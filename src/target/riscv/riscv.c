#include <assert.h>
#include <stdlib.h>
#include <time.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "target.h"
#include "target/algorithm.h"
#include "target_type.h"
#include "log.h"
#include "jtag/jtag.h"
#include "register.h"
#include "breakpoints.h"
#include "helper/time_support.h"
#include "riscv.h"
#include "gdb_regs.h"
#include "rtos/rtos.h"

/**
 * Since almost everything can be accomplish by scanning the dbus register, all
 * functions here assume dbus is already selected. The exception are functions
 * called directly by OpenOCD, which can't assume anything about what's
 * currently in IR. They should set IR to dbus explicitly.
 */

/**
 * Code structure
 *
 * At the bottom of the stack are the OpenOCD JTAG functions:
 * 		jtag_add_[id]r_scan
 * 		jtag_execute_query
 * 		jtag_add_runtest
 *
 * There are a few functions to just instantly shift a register and get its
 * value:
 * 		dtmcontrol_scan
 * 		idcode_scan
 * 		dbus_scan
 *
 * Because doing one scan and waiting for the result is slow, most functions
 * batch up a bunch of dbus writes and then execute them all at once. They use
 * the scans "class" for this:
 * 		scans_new
 * 		scans_delete
 * 		scans_execute
 * 		scans_add_...
 * Usually you new(), call a bunch of add functions, then execute() and look
 * at the results by calling scans_get...()
 *
 * Optimized functions will directly use the scans class above, but slightly
 * lazier code will use the cache functions that in turn use the scans
 * functions:
 * 		cache_get...
 * 		cache_set...
 * 		cache_write
 * cache_set... update a local structure, which is then synced to the target
 * with cache_write(). Only Debug RAM words that are actually changed are sent
 * to the target. Afterwards use cache_get... to read results.
 */

#define get_field(reg, mask) (((reg) & (mask)) / ((mask) & ~((mask) << 1)))
#define set_field(reg, mask, val) (((reg) & ~(mask)) | (((val) * ((mask) & ~((mask) << 1))) & (mask)))

#define DIM(x)		(sizeof(x)/sizeof(*x))

// Constants for legacy SiFive hardware breakpoints.
#define CSR_BPCONTROL_X			(1<<0)
#define CSR_BPCONTROL_W			(1<<1)
#define CSR_BPCONTROL_R			(1<<2)
#define CSR_BPCONTROL_U			(1<<3)
#define CSR_BPCONTROL_S			(1<<4)
#define CSR_BPCONTROL_H			(1<<5)
#define CSR_BPCONTROL_M			(1<<6)
#define CSR_BPCONTROL_BPMATCH	(0xf<<7)
#define CSR_BPCONTROL_BPACTION	(0xff<<11)

#define DEBUG_ROM_START         0x800
#define DEBUG_ROM_RESUME        (DEBUG_ROM_START + 4)
#define DEBUG_ROM_EXCEPTION     (DEBUG_ROM_START + 8)
#define DEBUG_RAM_START         0x400

#define SETHALTNOT				0x10c

/*** JTAG registers. ***/

#define DTMCONTROL					0x10
#define DTMCONTROL_DBUS_RESET		(1<<16)
#define DTMCONTROL_IDLE				(7<<10)
#define DTMCONTROL_ADDRBITS			(0xf<<4)
#define DTMCONTROL_VERSION			(0xf)

#define DBUS						0x11
#define DBUS_OP_START				0
#define DBUS_OP_SIZE				2
typedef enum {
	DBUS_OP_NOP = 0,
	DBUS_OP_READ = 1,
	DBUS_OP_WRITE = 2
} dbus_op_t;
typedef enum {
	DBUS_STATUS_SUCCESS = 0,
	DBUS_STATUS_FAILED = 2,
	DBUS_STATUS_BUSY = 3
} dbus_status_t;
#define DBUS_DATA_START				2
#define DBUS_DATA_SIZE				34
#define DBUS_ADDRESS_START			36

typedef enum {
	RE_OK,
	RE_FAIL,
	RE_AGAIN
} riscv_error_t;

typedef enum slot {
	SLOT0,
	SLOT1,
	SLOT_LAST,
} slot_t;

/*** Debug Bus registers. ***/

#define DMCONTROL				0x10
#define DMCONTROL_INTERRUPT		(((uint64_t)1)<<33)
#define DMCONTROL_HALTNOT		(((uint64_t)1)<<32)
#define DMCONTROL_BUSERROR		(7<<19)
#define DMCONTROL_SERIAL		(3<<16)
#define DMCONTROL_AUTOINCREMENT	(1<<15)
#define DMCONTROL_ACCESS		(7<<12)
#define DMCONTROL_HARTID		(0x3ff<<2)
#define DMCONTROL_NDRESET		(1<<1)
#define DMCONTROL_FULLRESET		1

#define DMINFO					0x11
#define DMINFO_ABUSSIZE			(0x7fU<<25)
#define DMINFO_SERIALCOUNT		(0xf<<21)
#define DMINFO_ACCESS128		(1<<20)
#define DMINFO_ACCESS64			(1<<19)
#define DMINFO_ACCESS32			(1<<18)
#define DMINFO_ACCESS16			(1<<17)
#define DMINFO_ACCESS8			(1<<16)
#define DMINFO_DRAMSIZE			(0x3f<<10)
#define DMINFO_AUTHENTICATED	(1<<5)
#define DMINFO_AUTHBUSY			(1<<4)
#define DMINFO_AUTHTYPE			(3<<2)
#define DMINFO_VERSION			3

/*** Info about the core being debugged. ***/

#define DBUS_ADDRESS_UNKNOWN	0xffff
#define WALL_CLOCK_TIMEOUT		2

// gdb's register list is defined in riscv_gdb_reg_names gdb/riscv-tdep.c in
// its source tree. We must interpret the numbers the same here.
enum {
	REG_XPR0 = 0,
	REG_XPR31 = 31,
	REG_PC = 32,
	REG_FPR0 = 33,
	REG_FPR31 = 64,
	REG_CSR0 = 65,
	REG_MSTATUS = CSR_MSTATUS + REG_CSR0,
	REG_CSR4095 = 4160,
	REG_PRIV = 4161,
	REG_COUNT
};

#define MAX_HWBPS			16
#define DRAM_CACHE_SIZE		16

uint8_t ir_dtmcontrol[1] = {DTMCONTROL};
struct scan_field select_dtmcontrol = {
       .in_value = NULL,
       .out_value = ir_dtmcontrol
};
uint8_t ir_dbus[1] = {DBUS};
struct scan_field select_dbus = {
       .in_value = NULL,
       .out_value = ir_dbus
};
uint8_t ir_idcode[1] = {0x1};
struct scan_field select_idcode = {
       .in_value = NULL,
       .out_value = ir_idcode
};

struct trigger {
	uint64_t address;
	uint32_t length;
	uint64_t mask;
	uint64_t value;
	bool read, write, execute;
	int unique_id;
};

static uint32_t dtmcontrol_scan(struct target *target, uint32_t out)
{
	struct scan_field field;
	uint8_t in_value[4];
	uint8_t out_value[4];

	buf_set_u32(out_value, 0, 32, out);

	jtag_add_ir_scan(target->tap, &select_dtmcontrol, TAP_IDLE);

	field.num_bits = 32;
	field.out_value = out_value;
	field.in_value = in_value;
	jtag_add_dr_scan(target->tap, 1, &field, TAP_IDLE);

	/* Always return to dbus. */
	jtag_add_ir_scan(target->tap, &select_dbus, TAP_IDLE);

	int retval = jtag_execute_queue();
	if (retval != ERROR_OK) {
		LOG_ERROR("failed jtag scan: %d", retval);
		return retval;
	}

	uint32_t in = buf_get_u32(field.in_value, 0, 32);
	LOG_DEBUG("DTMCONTROL: 0x%x -> 0x%x", out, in);

	return in;
}

static struct target_type *get_target_type(struct target *target)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;

	switch (info->dtm_version) {
		case 0:
			return &riscv011_target;
		case 1:
			return &riscv013_target;
		default:
			LOG_ERROR("Unsupported DTM version: %d", info->dtm_version);
			return NULL;
	}
}

static int riscv_init_target(struct command_context *cmd_ctx,
		struct target *target)
{
	LOG_DEBUG("riscv_init_target()");
	target->arch_info = calloc(1, sizeof(riscv_info_t));
	if (!target->arch_info)
		return ERROR_FAIL;
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	info->cmd_ctx = cmd_ctx;

	select_dtmcontrol.num_bits = target->tap->ir_length;
	select_dbus.num_bits = target->tap->ir_length;
	select_idcode.num_bits = target->tap->ir_length;

	return ERROR_OK;
}

static void riscv_deinit_target(struct target *target)
{
	LOG_DEBUG("riscv_deinit_target()");
	struct target_type *tt = get_target_type(target);
	tt->deinit_target(target);
	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	free(info);
	target->arch_info = NULL;
}

static int riscv_halt(struct target *target)
{
	struct target_type *tt = get_target_type(target);
	return tt->halt(target);
}

static int riscv_add_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	struct target_type *tt = get_target_type(target);
	return tt->add_breakpoint(target, breakpoint);
}

static int riscv_remove_breakpoint(struct target *target,
		struct breakpoint *breakpoint)
{
	struct target_type *tt = get_target_type(target);
	return tt->remove_breakpoint(target, breakpoint);
}

static int riscv_add_watchpoint(struct target *target,
		struct watchpoint *watchpoint)
{
	struct target_type *tt = get_target_type(target);
	return tt->add_watchpoint(target, watchpoint);
}

static int riscv_remove_watchpoint(struct target *target,
		struct watchpoint *watchpoint)
{
	struct target_type *tt = get_target_type(target);
	return tt->remove_watchpoint(target, watchpoint);
}

static int riscv_step(struct target *target, int current, uint32_t address,
		int handle_breakpoints)
{
	struct target_type *tt = get_target_type(target);
	return tt->step(target, current, address, handle_breakpoints);
}

static int riscv_examine(struct target *target)
{
	LOG_DEBUG("riscv_examine()");
	if (target_was_examined(target)) {
		return ERROR_OK;
	}

	// Don't need to select dbus, since the first thing we do is read dtmcontrol.

	riscv_info_t *info = (riscv_info_t *) target->arch_info;
	uint32_t dtmcontrol = dtmcontrol_scan(target, 0);
	LOG_DEBUG("dtmcontrol=0x%x", dtmcontrol);
	info->dtm_version = get_field(dtmcontrol, DTMCONTROL_VERSION);
	LOG_DEBUG("  version=0x%x", info->dtm_version);

	struct target_type *tt = get_target_type(target);
	if (tt == NULL)
		return ERROR_FAIL;

	int result = tt->init_target(info->cmd_ctx, target);
	if (result != ERROR_OK)
		return result;

	return tt->examine(target);
}

static int oldriscv_poll(struct target *target)
{
	struct target_type *tt = get_target_type(target);
	return tt->poll(target);
}

static int riscv_resume(struct target *target, int current, uint32_t address,
		int handle_breakpoints, int debug_execution)
{
	struct target_type *tt = get_target_type(target);
	return tt->resume(target, current, address, handle_breakpoints,
			debug_execution);
}

static int riscv_assert_reset(struct target *target)
{
	struct target_type *tt = get_target_type(target);
	return tt->assert_reset(target);
}

static int riscv_deassert_reset(struct target *target)
{
	struct target_type *tt = get_target_type(target);
	return tt->deassert_reset(target);
}

static int riscv_read_memory(struct target *target, uint32_t address,
		uint32_t size, uint32_t count, uint8_t *buffer)
{
	struct target_type *tt = get_target_type(target);
	return tt->read_memory(target, address, size, count, buffer);
}

static int riscv_write_memory(struct target *target, uint32_t address,
		uint32_t size, uint32_t count, const uint8_t *buffer)
{
	struct target_type *tt = get_target_type(target);
	return tt->write_memory(target, address, size, count, buffer);
}

static int riscv_get_gdb_reg_list(struct target *target,
		struct reg **reg_list[], int *reg_list_size,
		enum target_register_class reg_class)
{
	RISCV_INFO(r);
	LOG_DEBUG("reg_class=%d", reg_class);
	LOG_DEBUG("riscv_get_gdb_reg_list: rtos_hartid=%d current_hartid=%d", r->rtos_hartid, r->current_hartid);
	if (r->rtos_hartid != -1)
		riscv_set_current_hartid(target, r->rtos_hartid);
	else
		riscv_set_current_hartid(target, 0);

	switch (reg_class) {
		case REG_CLASS_GENERAL:
			*reg_list_size = 32;
			break;
		case REG_CLASS_ALL:
			*reg_list_size = REG_COUNT;
			break;
		default:
			LOG_ERROR("Unsupported reg_class: %d", reg_class);
			return ERROR_FAIL;
	}

	*reg_list = calloc(*reg_list_size, sizeof(struct reg *));
	if (!*reg_list) {
		return ERROR_FAIL;
	}
	for (int i = 0; i < *reg_list_size; i++) {
		assert(target->reg_cache->reg_list[i].size > 0);
		(*reg_list)[i] = &target->reg_cache->reg_list[i];
	}

	return ERROR_OK;
}

static int riscv_arch_state(struct target *target)
{
	struct target_type *tt = get_target_type(target);
	return tt->arch_state(target);
}

// Algorithm must end with a software breakpoint instruction.
static int riscv_run_algorithm(struct target *target, int num_mem_params,
		struct mem_param *mem_params, int num_reg_params,
		struct reg_param *reg_params, uint32_t entry_point,
		uint32_t exit_point, int timeout_ms, void *arch_info)
{
	riscv_info_t *info = (riscv_info_t *) target->arch_info;

	if (num_mem_params > 0) {
		LOG_ERROR("Memory parameters are not supported for RISC-V algorithms.");
		return ERROR_FAIL;
	}

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/// Save registers
	struct reg *reg_pc = register_get_by_name(target->reg_cache, "pc", 1);
	if (!reg_pc || reg_pc->type->get(reg_pc) != ERROR_OK) {
		return ERROR_FAIL;
	}
	uint64_t saved_pc = buf_get_u64(reg_pc->value, 0, reg_pc->size);

	uint64_t saved_regs[32];
	for (int i = 0; i < num_reg_params; i++) {
		LOG_DEBUG("save %s", reg_params[i].reg_name);
		struct reg *r = register_get_by_name(target->reg_cache, reg_params[i].reg_name, 0);
		if (!r) {
			LOG_ERROR("Couldn't find register named '%s'", reg_params[i].reg_name);
			return ERROR_FAIL;
		}

		if (r->size != reg_params[i].size) {
			LOG_ERROR("Register %s is %d bits instead of %d bits.",
					reg_params[i].reg_name, r->size, reg_params[i].size);
			return ERROR_FAIL;
		}

		if (r->number > REG_XPR31) {
			LOG_ERROR("Only GPRs can be use as argument registers.");
			return ERROR_FAIL;
		}

		if (r->type->get(r) != ERROR_OK) {
			return ERROR_FAIL;
		}
		saved_regs[r->number] = buf_get_u64(r->value, 0, r->size);
		if (r->type->set(r, reg_params[i].value) != ERROR_OK) {
			return ERROR_FAIL;
		}
	}


	// Disable Interrupts before attempting to run the algorithm.
	uint64_t current_mstatus;
	uint8_t mstatus_bytes[8];

	LOG_DEBUG("Disabling Interrupts");
	char mstatus_name[20];
	sprintf(mstatus_name, "csr%d", CSR_MSTATUS);
	struct reg *reg_mstatus = register_get_by_name(target->reg_cache,
			mstatus_name, 1);
	reg_mstatus->type->get(reg_mstatus);
	current_mstatus = buf_get_u64(reg_mstatus->value, 0, reg_mstatus->size);
	uint64_t ie_mask = MSTATUS_MIE | MSTATUS_HIE | MSTATUS_SIE | MSTATUS_UIE;
	buf_set_u64(mstatus_bytes, 0, info->xlen[0], set_field(current_mstatus,
				ie_mask, 0));

	reg_mstatus->type->set(reg_mstatus, mstatus_bytes);

	/// Run algorithm
	LOG_DEBUG("resume at 0x%x", entry_point);
	if (riscv_resume(target, 0, entry_point, 0, 0) != ERROR_OK) {
		return ERROR_FAIL;
	}

	int64_t start = timeval_ms();
	while (target->state != TARGET_HALTED) {
		LOG_DEBUG("poll()");
		int64_t now = timeval_ms();
		if (now - start > timeout_ms) {
			LOG_ERROR("Algorithm timed out after %d ms.", timeout_ms);
			riscv_halt(target);
			oldriscv_poll(target);
			return ERROR_TARGET_TIMEOUT;
		}

		int result = oldriscv_poll(target);
		if (result != ERROR_OK) {
			return result;
		}
	}

	if (reg_pc->type->get(reg_pc) != ERROR_OK) {
		return ERROR_FAIL;
	}
	uint64_t final_pc = buf_get_u64(reg_pc->value, 0, reg_pc->size);
	if (final_pc != exit_point) {
		LOG_ERROR("PC ended up at 0x%" PRIx64 " instead of 0x%" PRIx32,
				final_pc, exit_point);
		return ERROR_FAIL;
	}

	// Restore Interrupts
	LOG_DEBUG("Restoring Interrupts");
	buf_set_u64(mstatus_bytes, 0, info->xlen[0], current_mstatus);
	reg_mstatus->type->set(reg_mstatus, mstatus_bytes);

	/// Restore registers
	uint8_t buf[8];
	buf_set_u64(buf, 0, info->xlen[0], saved_pc);
	if (reg_pc->type->set(reg_pc, buf) != ERROR_OK) {
		return ERROR_FAIL;
	}

	for (int i = 0; i < num_reg_params; i++) {
		LOG_DEBUG("restore %s", reg_params[i].reg_name);
		struct reg *r = register_get_by_name(target->reg_cache, reg_params[i].reg_name, 0);
		buf_set_u64(buf, 0, info->xlen[0], saved_regs[r->number]);
		if (r->type->set(r, buf) != ERROR_OK) {
			return ERROR_FAIL;
		}
	}

	return ERROR_OK;
}

/* Should run code on the target to perform CRC of 
memory. Not yet implemented.
*/

static int riscv_checksum_memory(struct target *target,
			  uint32_t address, uint32_t count,
			  uint32_t* checksum)
{
  *checksum = 0xFFFFFFFF;
  return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
}

/* Should run code on the target to check whether a memory
block holds all-ones (because this is generally called on
NOR flash which is 1 when "blank")
Not yet implemented.
*/
int riscv_blank_check_memory(struct target * target,
			    uint32_t address,
			    uint32_t count,
			    uint32_t * blank)
{
  *blank = 0;

  return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
}

struct target_type riscv_target =
{
	.name = "riscv",

	.init_target = riscv_init_target,
	.deinit_target = riscv_deinit_target,
	.examine = riscv_examine,

	/* poll current target status */
	.poll = oldriscv_poll,

	.halt = riscv_halt,
	.resume = riscv_resume,
	.step = riscv_step,

	.assert_reset = riscv_assert_reset,
	.deassert_reset = riscv_deassert_reset,

	.read_memory = riscv_read_memory,
	.write_memory = riscv_write_memory,

	.blank_check_memory = riscv_blank_check_memory,
	.checksum_memory = riscv_checksum_memory,

	.get_gdb_reg_list = riscv_get_gdb_reg_list,

	.add_breakpoint = riscv_add_breakpoint,
	.remove_breakpoint = riscv_remove_breakpoint,

	.add_watchpoint = riscv_add_watchpoint,
	.remove_watchpoint = riscv_remove_watchpoint,

	.arch_state = riscv_arch_state,

	.run_algorithm = riscv_run_algorithm,
};

/*** OpenOCD Helper Functions ***/

/* 0 means nothing happened, 1 means the hart's state changed (and thus the
 * poll should terminate), and -1 means there was an error. */
static int riscv_poll_hart(struct target *target, int hartid)
{
	RISCV_INFO(r);
	LOG_DEBUG("polling hart %d", hartid);

	/* If there's no new event then there's nothing to do. */
	riscv_set_current_hartid(target, hartid);
	assert((riscv_was_halted(target) && riscv_is_halted(target)) || !riscv_was_halted(target));
	if (riscv_was_halted(target) || !riscv_is_halted(target))
		return 0;

	/* If we got here then this must be the first poll during which this
	 * hart halted.  We need to synchronize the hart's state with the
	 * debugger, and inform the outer polling loop that there's something
	 * to do. */
	r->hart_state[hartid] = RISCV_HART_HALTED;
	r->on_halt(target);
	return 1;
}

/*** OpenOCD Interface ***/
int riscv_openocd_poll(struct target *target)
{
	LOG_DEBUG("polling all harts");
	if (riscv_rtos_enabled(target)) {
		/* Check every hart for an event. */
		int triggered_hart = -1;
		for (int i = 0; i < riscv_count_harts(target); ++i) {
			int out = riscv_poll_hart(target, i);
			switch (out) {
			case 0:
				continue;
			case 1:
				triggered_hart = i;
				break;
			case -1:
				return ERROR_FAIL;
			}
		}
		if (triggered_hart == -1) {
			LOG_DEBUG("  no harts halted");
			return ERROR_OK;
		}
		LOG_DEBUG("  hart %d halted", triggered_hart);

		/* If we're here then at least one hart triggered.  That means
		 * we want to go and halt _every_ hart in the system, as that's
		 * the invariant we hold here.  Some harts might have already
		 * halted (as we're either in single-step mode or they also
		 * triggered a breakpoint), so don't attempt to halt those
		 * harts. */
		for (int i = 0; i < riscv_count_harts(target); ++i)
			riscv_halt_one_hart(target, i);

		target->state = TARGET_HALTED;
		switch (riscv_halt_reason(target, triggered_hart)) {
		case RISCV_HALT_BREAKPOINT:
			target->debug_reason = DBG_REASON_BREAKPOINT;
			break;
		case RISCV_HALT_INTERRUPT:
			target->debug_reason = DBG_REASON_DBGRQ;
			break;
		case RISCV_HALT_SINGLESTEP:
			target->debug_reason = DBG_REASON_SINGLESTEP;
			break;
		}
		
		target->rtos->current_threadid = triggered_hart + 1;
		target->rtos->current_thread = triggered_hart + 1;

		target_call_event_callbacks(target, TARGET_EVENT_HALTED);
		return ERROR_OK;
	} else {
		return riscv_poll_hart(target, riscv_current_hartid(target));
	}
}

int riscv_openocd_halt(struct target *target)
{
	int out = riscv_halt_all_harts(target);
	if (out != ERROR_OK)
		return out;

	target->state = TARGET_HALTED;
	return out;
}

int riscv_openocd_resume(
        struct target *target,
        int current,
        uint32_t address,
        int handle_breakpoints,
        int debug_execution
) {
	if (!current) {
		LOG_ERROR("resume-at-pc unimplemented");
		return ERROR_FAIL;
	}

	int out = riscv_resume_all_harts(target);
	if (out != ERROR_OK)
		return out;

	target->state = TARGET_RUNNING;
	return out;
}

int riscv_openocd_step(
        struct target *target,
        int current,
        uint32_t address,
        int handle_breakpoints
) {
	if (!current) {
		LOG_ERROR("step-at-pc unimplemented");
		return ERROR_FAIL;
	}

	int out = riscv_step_rtos_hart(target);
	if (out != ERROR_OK)
		return out;

	target->state = TARGET_RUNNING;
	return out;
}

/*** RISC-V Interface ***/

void riscv_info_init(riscv_info_t *r)
{
	memset(r, 0, sizeof(*r));
	r->dtm_version = 1;

	/* FIXME: The RTOS gets enabled before the target gets initialized. */
	r->rtos_enabled = true;

	for (size_t h = 0; h < RISCV_MAX_HARTS; ++h) {
		/* FIXME: I need to rip out Tim's probing sequence, as it
		 * disrupts the running code.  For now, I'm just hard-coding
		 * XLEN to 64 for all cores at reset. */
		r->xlen[h] = 64;
		r->hart_state[h] = RISCV_HART_UNKNOWN;

		for (size_t e = 0; e < RISCV_MAX_REGISTERS; ++e)
			r->valid_saved_registers[h][e] = false;
	}
}

void riscv_save_register(struct target *target, int regno)
{
	RISCV_INFO(r);
	int hartno = r->current_hartid;
	LOG_DEBUG("riscv_save_register(%d, %d)", hartno, regno);
	assert(r->valid_saved_registers[hartno][regno] == false);
	r->valid_saved_registers[hartno][regno] = true;
	r->saved_registers[hartno][regno] = riscv_get_register(target, hartno, regno);
}

uint64_t riscv_peek_register(struct target *target, int regno)
{
	RISCV_INFO(r);
	int hartno = r->current_hartid;
	LOG_DEBUG("riscv_peek_register(%d, %d)", hartno, regno);
	assert(r->valid_saved_registers[hartno][regno] == true);
	return r->saved_registers[hartno][regno];
}

void riscv_overwrite_register(struct target *target, int regno, uint64_t newval)
{
	RISCV_INFO(r);
	int hartno = r->current_hartid;
	LOG_DEBUG("riscv_overwrite_register(%d, %d)", hartno, regno);
	assert(r->valid_saved_registers[hartno][regno] == true);
	r->saved_registers[hartno][regno] = newval;
}

void riscv_restore_register(struct target *target, int regno)
{
	RISCV_INFO(r);
	int hartno = r->current_hartid;
	LOG_DEBUG("riscv_restore_register(%d, %d)", hartno, regno);
	assert(r->valid_saved_registers[hartno][regno] == true);
	r->valid_saved_registers[hartno][regno] = false;
	riscv_set_register(target, hartno, regno, r->saved_registers[hartno][regno]);
}

int riscv_halt_all_harts(struct target *target)
{
	if (riscv_rtos_enabled(target)) {
		for (int i = 0; i < riscv_count_harts(target); ++i)
			riscv_halt_one_hart(target, i);
	} else {
		riscv_halt_one_hart(target, riscv_current_hartid(target));
	}

	return ERROR_OK;
}

int riscv_halt_one_hart(struct target *target, int hartid)
{
	RISCV_INFO(r);
	LOG_DEBUG("halting hart %d", hartid);
	riscv_set_current_hartid(target, hartid);

	if (r->hart_state[hartid] == RISCV_HART_UNKNOWN) {
		r->hart_state[hartid] = riscv_is_halted(target) ? RISCV_HART_HALTED : RISCV_HART_RUNNING;
		if (riscv_was_halted(target)) {
			LOG_WARNING("Connected to hart %d, which was halted.  s0, s1, and pc were overwritten by your previous debugger session and cannot be restored.", hartid);
			r->on_halt(target);
		}
	}

	if (riscv_was_halted(target)) {
		LOG_DEBUG("  hart %d requested halt, but was already halted", hartid);
		return ERROR_OK;
	}

	r->halt_current_hart(target);
	return ERROR_OK;
}

int riscv_resume_all_harts(struct target *target)
{
	if (riscv_rtos_enabled(target)) {
		for (int i = 0; i < riscv_count_harts(target); ++i)
			riscv_resume_one_hart(target, i);
	} else {
		riscv_resume_one_hart(target, riscv_current_hartid(target));
	}

	return ERROR_OK;
}

int riscv_resume_one_hart(struct target *target, int hartid)
{
	RISCV_INFO(r);
	LOG_DEBUG("resuming hart %d", hartid);
	riscv_set_current_hartid(target, hartid);

	if (r->hart_state[hartid] == RISCV_HART_UNKNOWN) {
		r->hart_state[hartid] = riscv_is_halted(target) ? RISCV_HART_HALTED : RISCV_HART_RUNNING;
		if (!riscv_was_halted(target)) {
			LOG_ERROR("Asked to resume hart %d, which was in an unknown state", hartid);
			r->on_resume(target);
		}
	}

	if (!riscv_was_halted(target)) {
		LOG_DEBUG("  hart %d requested resume, but was already resumed", hartid);
		return ERROR_OK;
	}

	r->on_resume(target);
	r->resume_current_hart(target);
	r->hart_state[hartid] = RISCV_HART_RUNNING;
	return ERROR_OK;
}

int riscv_step_rtos_hart(struct target *target)
{
	RISCV_INFO(r);
	int hartid = r->current_hartid;
	if (riscv_rtos_enabled(target)) {
		hartid = r->rtos_hartid;
		if (hartid == -1) {
			LOG_USER("GDB has asked me to step \"any\" thread, so I'm stepping hart 0.");
			hartid = 0;
		}
	}
	riscv_set_current_hartid(target, hartid);
	LOG_DEBUG("stepping hart %d", hartid);

	assert(r->hart_state[hartid] == RISCV_HART_HALTED);
	r->on_step(target);
	r->step_current_hart(target);
	/* FIXME: There's a race condition with step. */
	r->hart_state[hartid] = RISCV_HART_RUNNING;
	return ERROR_OK;
}

int riscv_xlen(const struct target *target)
{
	return riscv_xlen_of_hart(target, riscv_current_hartid(target));
}

int riscv_xlen_of_hart(const struct target *target, int hartid)
{
	RISCV_INFO(r);
	assert(r->xlen[hartid] != -1);
	return r->xlen[hartid];
}

void riscv_enable_rtos(struct target *target)
{
	RISCV_INFO(r);
	r->rtos_enabled = true;
}

bool riscv_rtos_enabled(const struct target *target)
{
	RISCV_INFO(r);
	return r->rtos_enabled;
}

void riscv_set_current_hartid(struct target *target, int hartid)
{
	RISCV_INFO(r);
	register_cache_invalidate(target->reg_cache);
	r->current_hartid = hartid;
	r->select_current_hart(target);

	/* Update the register list's widths. */
	for (size_t i = 0; i < GDB_REGNO_COUNT; ++i) {
		struct reg *reg = &target->reg_cache->reg_list[i];

		reg->value = &r->reg_cache_values[i];
		reg->valid = false;

		switch (i) {
		case GDB_REGNO_PRIV:
			reg->size = 8;
			break;
		default:
			reg->size = riscv_xlen(target);
			break;
		}
	}
}

int riscv_current_hartid(const struct target *target)
{
	RISCV_INFO(r);
	assert(riscv_rtos_enabled(target) || target->coreid == r->current_hartid);
	return r->current_hartid;
}

void riscv_set_all_rtos_harts(struct target *target)
{
	RISCV_INFO(r);
	r->rtos_hartid = -1;
}

void riscv_set_rtos_hartid(struct target *target, int hartid)
{
	RISCV_INFO(r);
	r->rtos_hartid = hartid;
}

int riscv_count_harts(struct target *target)
{
	return 3;
}

bool riscv_has_register(struct target *target, int hartid, int regid)
{
	return 1;
}

void riscv_set_register(struct target *target, int hartid, enum gdb_regno regid, uint64_t value)
{
	RISCV_INFO(r);
	LOG_DEBUG("writing register %d on hart %d", regid, hartid);
	return r->set_register(target, hartid, regid, value);
}

uint64_t riscv_get_register(struct target *target, int hartid, enum gdb_regno regid)
{
	RISCV_INFO(r);
	LOG_DEBUG("reading register %d on hart %d", regid, hartid);
	return r->get_register(target, hartid, regid);
}

bool riscv_is_halted(struct target *target)
{
	RISCV_INFO(r);
	return r->is_halted(target);
}

bool riscv_was_halted(struct target *target)
{
	RISCV_INFO(r);
	assert(r->hart_state[r->current_hartid] != RISCV_HART_UNKNOWN);
	return r->hart_state[r->current_hartid] == RISCV_HART_HALTED;
}

enum riscv_halt_reason riscv_halt_reason(struct target *target, int hartid)
{
	RISCV_INFO(r);
	riscv_set_current_hartid(target, hartid);
	assert(riscv_is_halted(target));
	return r->halt_reason(target);
}