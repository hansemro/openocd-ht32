
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "../common.h"
#include "../../target/target.h"
#include "../../helper/command.h"
#include "../../helper/log.h"

#include "imp.h"
#include <target/armv7m.h>

#define FMC_REG_BASE        0x40080000
#define FMC_REG_TADR        0x00
#define FMC_REG_WRDR        0x04
#define FMC_REG_OCMR        0x0C
#define FMC_REG_OPCR        0x10
#define FMC_REG_PPSR        0x20
#define FMC_REG_CPSR        0x30

#define FMC_CMD_WORD_PROG   0x4
#define FMC_CMD_PAGE_ERASE  0x8
#define FMC_CMD_MASS_ERASE  0xA

#define FMC_OPM_MASK        0x1E
#define FMC_COMMIT          (0xA << 1)
#define FMC_FINISHED        (0xE << 1)

#define FLASH_ERASE_TIMEOUT 1000

#define OPT_BYTE            0x1FF00000

// flash bank ht32f165x <base> <size> 0 0 <target#>
FLASH_BANK_COMMAND_HANDLER(ht32f165x_flash_bank_command)
{
    if (CMD_ARGC < 6)
        return ERROR_COMMAND_SYNTAX_ERROR;

    bank->driver_priv = NULL;

    return ERROR_OK;
}

static int ht32f165x_get_flash_status(struct flash_bank *bank, uint32_t *status)
{
    struct target *target = bank->target;
    return target_read_u32(target, FMC_REG_BASE + FMC_REG_OPCR, status);
}

static int ht32f165x_wait_status_busy(struct flash_bank *bank, int timeout)
{
    uint32_t status;
    int retval = ERROR_OK;

    // wait for flash operation finished
    for (;;) {
        retval = ht32f165x_get_flash_status(bank, &status);
        if (retval != ERROR_OK)
            return retval;

        if ((status & FMC_OPM_MASK) == FMC_FINISHED)
            return ERROR_OK;

        if (timeout-- <= 0) {
            LOG_ERROR("Timed out waiting for flash: 0x%04x", status);
            return ERROR_FAIL;
        }
        alive_sleep(10);
    }

    return retval;
}

static int ht32f165x_erase(struct flash_bank *bank, unsigned int first, unsigned int last)
{
    struct target *target = bank->target;

    LOG_DEBUG("ht32f165x erase: %d - %d", first, last);

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    for(unsigned int i = first ; i <= last; ++i){
        // flash memory page erase
        int retval = target_write_u32(target, FMC_REG_BASE + FMC_REG_TADR, bank->sectors[i].offset);
        if (retval != ERROR_OK)
            return retval;
        retval = target_write_u32(target, FMC_REG_BASE + FMC_REG_OCMR, FMC_CMD_PAGE_ERASE);
        if (retval != ERROR_OK)
            return retval;
        retval = target_write_u32(target, FMC_REG_BASE + FMC_REG_OPCR, FMC_COMMIT);
        if (retval != ERROR_OK)
            return retval;

        // wait
        retval = ht32f165x_wait_status_busy(bank, FLASH_ERASE_TIMEOUT);
        if (retval != ERROR_OK)
            return retval;

        LOG_DEBUG("ht32f165x erased page %d", i);
        bank->sectors[i].is_erased = 1;
    }

    return ERROR_OK;
}

static int ht32f165x_program_word(struct flash_bank *bank, uint32_t addr, uint32_t value)
{
    struct target *target = bank->target;
    int retval;
    LOG_INFO("ht32f165x programming word 0x%04x @ 0x%04x", value, addr);
    retval = target_write_u32(target, FMC_REG_BASE + FMC_REG_TADR, addr);
    if (retval != ERROR_OK)
        return retval;
    retval = target_write_u32(target, FMC_REG_BASE + FMC_REG_WRDR, value);
    if (retval != ERROR_OK)
        return retval;
    retval = target_write_u32(target, FMC_REG_BASE + FMC_REG_OCMR, FMC_CMD_WORD_PROG);
    if (retval != ERROR_OK)
        return retval;
    retval = target_write_u32(target, FMC_REG_BASE + FMC_REG_OPCR, FMC_COMMIT);
    if (retval != ERROR_OK)
        return retval;

    // wait
    retval = ht32f165x_wait_status_busy(bank, FLASH_ERASE_TIMEOUT);
    if (retval != ERROR_OK)
        return retval;
    return ERROR_OK;
}

static int ht32f165x_protect(struct flash_bank *bank, int set, unsigned int first, unsigned int last)
{
    struct target *target = bank->target;
    uint32_t security;
    uint32_t ob_pp[4];
    uint32_t ob_cp;
    uint32_t ob_ck;

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    // skip operation if security is already enabled
    // flash security cannot be unset once set and can only
    // be cleared by mass erase
    target_read_u32(target, FMC_REG_BASE + FMC_REG_CPSR, &security);
    if ((security & 1) == 0 || set == 0)
        return ERROR_FLASH_OPER_UNSUPPORTED;

    // erase option byte page
    int retval = target_write_u32(target, FMC_REG_BASE + FMC_REG_TADR, OPT_BYTE);
    if (retval != ERROR_OK)
        return retval;
    retval = target_write_u32(target, FMC_REG_BASE + FMC_REG_OCMR, FMC_CMD_PAGE_ERASE);
    if (retval != ERROR_OK)
        return retval;
    retval = target_write_u32(target, FMC_REG_BASE + FMC_REG_OPCR, FMC_COMMIT);
    if (retval != ERROR_OK)
        return retval;
    // wait
    retval = ht32f165x_wait_status_busy(bank, FLASH_ERASE_TIMEOUT);
    if (retval != ERROR_OK)
        return retval;

    // enable security protection
    ob_cp = ~(1 | 0 << 1);
    ob_ck = ob_cp;

    // enable write protection
    ob_pp[0] = 0x00000000;
    ob_pp[1] = 0xffffffff;
    ob_pp[2] = 0xffffffff;
    ob_pp[3] = 0xffffffff;
    ob_ck += ob_pp[0] + ob_pp[1] + ob_pp[2] + ob_pp[3];

    // program option byte
    for (int i = 0; i < 4; i++) {
        retval = ht32f165x_program_word(bank, OPT_BYTE + (i << 2), ob_pp[i]);
        if (retval != ERROR_OK)
            break;
    }
    if (retval == ERROR_OK)
        retval = ht32f165x_program_word(bank, OPT_BYTE + 0x10, ob_cp);
    if (retval == ERROR_OK)
        retval = ht32f165x_program_word(bank, OPT_BYTE + 0x20, ob_ck);

    if (retval == ERROR_OK) {
        // instruct user to do a system reset
        LOG_INFO("ht32f165x Security will be set on reset");
    } else {
        LOG_ERROR("ht32f165x Failed to program option bytes");
        return retval;
    }

    return ERROR_OK;
}

COMMAND_HANDLER(ht32f165x_handle_enable_security)
{
    if (CMD_ARGC < 1)
        return ERROR_COMMAND_SYNTAX_ERROR;

    struct flash_bank *bank;
    int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
    if (retval != ERROR_OK)
        return retval;

    retval = ht32f165x_protect(bank, 1, 0, 0);
    if (retval == ERROR_OK) {
        command_print(CMD, "ht32f165x enable security complete");
    } else {
        command_print(CMD, "ht32f165x enable security failed");
    }

    return retval;
}

static int ht32f165x_write(struct flash_bank *bank, const uint8_t *buffer,
                           uint32_t offset, uint32_t count)
{
    struct target *target = bank->target;

    LOG_DEBUG("ht32f165x flash write: 0x%x 0x%x", offset, count);

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    if(offset & 0x3){
        LOG_ERROR("offset 0x%" PRIx32 " breaks required 4-byte alignment", offset);
        return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
    }

    if(count & 0x3){
        LOG_ERROR("size 0x%" PRIx32 " breaks required 4-byte alignment", count);
        return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
    }

    uint32_t addr = offset;
    for(uint32_t i = 0; i < count; i += 4){
        uint32_t word = (buffer[i]   << 0) |
                        (buffer[i+1] << 8) |
                        (buffer[i+2] << 16) |
                        (buffer[i+3] << 24);

        LOG_DEBUG("ht32f165x flash write word 0x%x 0x%x 0x%08x", i, addr, word);

        // flash memory word program
        int retval;
        retval = target_write_u32(target, FMC_REG_BASE + FMC_REG_TADR, addr);
        if (retval != ERROR_OK)
            return retval;
        retval = target_write_u32(target, FMC_REG_BASE + FMC_REG_WRDR, word);
        if (retval != ERROR_OK)
            return retval;
        retval = target_write_u32(target, FMC_REG_BASE + FMC_REG_OCMR, FMC_CMD_WORD_PROG);
        if (retval != ERROR_OK)
            return retval;
        retval = target_write_u32(target, FMC_REG_BASE + FMC_REG_OPCR, FMC_COMMIT);
        if (retval != ERROR_OK)
            return retval;

        // wait
        retval = ht32f165x_wait_status_busy(bank, FLASH_ERASE_TIMEOUT);
        if (retval != ERROR_OK)
            return retval;
        addr += 4;
    }

    LOG_DEBUG("ht32f165x flash write success");
    return ERROR_OK;
}

static int ht32f165x_security_check(struct flash_bank *bank)
{
    struct target *target = bank->target;
    uint32_t security;
    uint32_t ob_cp;
    uint32_t ob_ck;
    target_read_u32(target, FMC_REG_BASE + FMC_REG_CPSR, &security);
    LOG_INFO("ht32f165x CPSR: 0x%04x", security);
    target_read_u32(target, OPT_BYTE + 0x10, &ob_cp);
    LOG_INFO("ht32f165x OB_CP: 0x%04x", ob_cp);
    target_read_u32(target, OPT_BYTE + 0x20, &ob_ck);
    LOG_INFO("ht32f165x OB_CK: 0x%04x", ob_ck);
    return ERROR_OK;
}

static int ht32f165x_protect_check(struct flash_bank *bank)
{
    struct target *target = bank->target;
    uint32_t ob_pp[4];
    uint32_t ob_cp;

    // Read page protection
    for(int i = 0; i < 4; ++i)
        target_read_u32(target, FMC_REG_BASE + FMC_REG_PPSR + (i << 2), ob_pp + i);
    // Read protection config
    target_read_u32(target, FMC_REG_BASE + FMC_REG_CPSR, &ob_cp);

    LOG_INFO("ht32f165x opt byte: %04x %04x %04x %04x %04x", ob_pp[0], ob_pp[1], ob_pp[2], ob_pp[3], ob_cp);

    // Set page protection
    for(int i = 0 ; i < 128; ++i){
        size_t index = i / (sizeof(uint32_t) * 8);
        size_t offset = i % (sizeof(uint32_t) * 8);
        uint8_t is_protected = (ob_pp[index] & (1U << offset)) == 0;
        bank->sectors[i].is_protected = is_protected;
    }

    return ERROR_OK;
}

static int ht32f165x_probe(struct flash_bank *bank)
{
    int page_size = 1024;
    int num_pages = bank->size / page_size;

    LOG_INFO("ht32f165x probe: %d pages, 0x%x bytes, 0x%x total", num_pages, page_size, bank->size);

    if(bank->sectors)
        free(bank->sectors);

    bank->base = 0x0;
    bank->num_sectors = num_pages;
    bank->sectors = malloc(sizeof(struct flash_sector) * num_pages);

    for(int i = 0; i < num_pages; ++i){
        bank->sectors[i].offset = i * page_size;
        bank->sectors[i].size = page_size;
        bank->sectors[i].is_erased = -1;
        bank->sectors[i].is_protected = 1;
    }

    ht32f165x_protect_check(bank);

    return ERROR_OK;
}

static int ht32f165x_auto_probe(struct flash_bank *bank)
{
    return ht32f165x_probe(bank);
}

static int ht32f165x_info(struct flash_bank *bank, struct command_invocation *cmd)
{
    ht32f165x_probe(bank);

    const char *info = "ht32f165x flash";
    command_print_sameline(cmd, "%s", info);
    return ERROR_OK;
}

static int ht32f165x_check_security(struct flash_bank *bank)
{
    struct target *target = bank->target;

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    ht32f165x_security_check(bank);
    ht32f165x_protect_check(bank);

    return ERROR_OK;
}

COMMAND_HANDLER(ht32f165x_handle_check_security)
{
    if (CMD_ARGC < 1)
        return ERROR_COMMAND_SYNTAX_ERROR;

    struct flash_bank *bank;
    int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
    if (retval != ERROR_OK)
        return retval;

    retval = ht32f165x_check_security(bank);
    if (retval == ERROR_OK) {
        command_print(CMD, "ht32f165x check_security complete");
    } else {
        command_print(CMD, "ht32f165x check_security failed");
    }

    return retval;
}

static int ht32f165x_mass_erase(struct flash_bank *bank)
{
    struct target *target = bank->target;

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    // flash memory mass erase
    int retval = target_write_u32(target, FMC_REG_BASE + FMC_REG_OCMR, FMC_CMD_MASS_ERASE);
    if (retval != ERROR_OK)
        return retval;
    retval = target_write_u32(target, FMC_REG_BASE + FMC_REG_OPCR, FMC_COMMIT);
    if (retval != ERROR_OK)
        return retval;

    retval = ht32f165x_wait_status_busy(bank, FLASH_ERASE_TIMEOUT);
    if (retval != ERROR_OK)
        return retval;

    return ERROR_OK;
}

COMMAND_HANDLER(ht32f165x_handle_mass_erase_command)
{
    if (CMD_ARGC < 1)
        return ERROR_COMMAND_SYNTAX_ERROR;

    struct flash_bank *bank;
    int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
    if (retval != ERROR_OK)
        return retval;

    retval = ht32f165x_mass_erase(bank);
    if (retval == ERROR_OK) {
        // set all sectors as erased
        unsigned int i;
        for (i = 0; i < bank->num_sectors; i++)
            bank->sectors[i].is_erased = 1;

        command_print(CMD, "ht32f165x mass erase complete");
    } else {
        command_print(CMD, "ht32f165x mass erase failed");
    }

    return retval;
}

COMMAND_HANDLER(ht32f165x_handle_test_write)
{
    if (CMD_ARGC < 1)
        return ERROR_COMMAND_SYNTAX_ERROR;

    struct flash_bank *bank;
    int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
    if (retval != ERROR_OK)
        return retval;

    uint8_t buffer[32];
    for(int i = 0; i < 32; ++i){
        buffer[i] = i;
    }

    retval = ht32f165x_erase(bank, 0, 0);
    if (retval != ERROR_OK)
        return retval;

    retval = ht32f165x_write(bank, buffer, 0, 32);
    if (retval == ERROR_OK) {
        command_print(CMD, "ht32f165x test write complete");
    } else {
        command_print(CMD, "ht32f165x test write failed");
    }

    return retval;
}


static const struct command_registration ht32f165x_exec_command_handlers[] = {
    {
        .name = "mass_erase",
        .handler = ht32f165x_handle_mass_erase_command,
        .mode = COMMAND_EXEC,
        .usage = "bank_id",
        .help = "erase entire flash device",
    },
    {
        .name = "test_write",
        .handler = ht32f165x_handle_test_write,
        .mode = COMMAND_EXEC,
        .usage = "bank_id",
        .help = " test flash write",
    },
    {
        .name = "check_security",
        .handler = ht32f165x_handle_check_security,
        .mode = COMMAND_EXEC,
        .usage = "bank_id",
        .help = " check flash security",
    },
    {
        .name = "enable_security",
        .handler = ht32f165x_handle_enable_security,
        .mode = COMMAND_EXEC,
        .usage = "bank_id",
        .help = " enable flash security",
    },
    COMMAND_REGISTRATION_DONE
};

static const struct command_registration ht32f165x_command_handlers[] = {
    {
        .name = "ht32f165x",
        .mode = COMMAND_ANY,
        .help = "ht32f165x flash command group",
        .usage = "",
        .chain = ht32f165x_exec_command_handlers,
    },
    COMMAND_REGISTRATION_DONE
};

struct flash_driver ht32f165x_flash = {
    .name = "ht32f165x",
    .commands = ht32f165x_command_handlers,
    .flash_bank_command = ht32f165x_flash_bank_command,

    .erase          = ht32f165x_erase,
    .protect        = ht32f165x_protect,
    .write          = ht32f165x_write,
    .read           = default_flash_read,
    .probe          = ht32f165x_probe,
    .auto_probe     = ht32f165x_auto_probe,
    .erase_check    = default_flash_blank_check,
    .protect_check  = ht32f165x_protect_check,
    .info           = ht32f165x_info,
};
