/* main.c: GBFlash open firmware, main loop. Recovery from an image that does
 * not enumerate is U22 held at power-on. */

#include <stdint.h>
#include "fw_config.h"
#include "proto.h"
#include "cart.h"
#include "usb.h"
#include "timebase.h"
#include "lk_glue.h"

/* g_reply is 16 KB at FW_MAX_TRANSFER against 1 KB of stack headroom; as an
 * automatic it runs the stack into .bss. */
/* BL_USB_ECHO=1 loops the receive path back and swallows BOOTLOADER_RESET. */
#if !defined(BL_USB_ECHO) || BL_USB_ECHO != 0
#error "BL_USB_ECHO must be defined as 0 for the firmware build. See the Makefile."
#endif

static uint8_t g_reply[FW_MAX_TRANSFER] __attribute__((aligned(4)));

#if FW_DMG_PROFILE
/* SysTick is free running, 24 bit, counting down at Fsys. One lap is 419 ms at
 * 40 MHz, so a buffer load never wraps it. */
uint32_t g_prof_loads, g_prof_cyc_load, g_prof_cyc_poll, g_prof_poll_iters;
uint32_t g_prof_pump_loads, g_prof_cyc_pump;
uint32_t g_prof_cmds, g_prof_cyc_cmd;
uint32_t g_prof_rd_bytes, g_prof_cyc_rd_bus, g_prof_cyc_rd_pub;

/* Stack high-water. The free region runs from __heap_end up to __stack_top;
 * fill it once at startup, below this frame, and count back from the top to the
 * first word still holding the pattern. */
extern uint32_t __heap_end;

#define PROF_STACK_PAT  0xC0DEC0DEu

void fw_prof_stack_fill(void)
{
    uint32_t sp;
    uint32_t *p = &__heap_end;
    uint32_t *end;

    __asm__ volatile ("mov %0, sp" : "=r" (sp));
    end = (uint32_t *)(uintptr_t)((sp - 256u) & ~3u);
    while (p < end) {
        *p++ = PROF_STACK_PAT;
    }
}

uint32_t fw_prof_stack_highwater(void)
{
    const uint32_t *p = &__heap_end;
    const uint32_t *top = (const uint32_t *)(uintptr_t)0x20008000u;

    while (p < top && *p == PROF_STACK_PAT) {
        p++;
    }
    return (uint32_t)((uintptr_t)top - (uintptr_t)p);
}

static uint32_t g_prof_in_pump;

static inline uint32_t prof_now(void)
{
    return (*(volatile uint32_t *)0xE000E018u) & 0x00FFFFFFu;
}

static inline uint32_t prof_delta(uint32_t start)
{
    return (start - prof_now()) & 0x00FFFFFFu;
}
#endif

static fw_state_t g_state;

/* v1.3 only. PB22 is the strap but also the cart VCC enable, and v1.2 needs the
 * bus pre-charge at cart.c:494, which is not implemented. */
static uint8_t probe_pcb_version(void)
{
    return 13u;
}

/* Magic word then reset; bl_boot() clears it, so update mode cannot trap.
 * Pump USB first or the ACK never leaves and the host reports a failure. */
static void enter_update_mode(void)
{
    uint32_t deadline = bl_time_ms() + 20u;
    uint32_t guard = 400000u;

    /* Backstop against a stopped timebase; ~3.5 s at 32 MHz. */
    while ((int32_t)(bl_time_ms() - deadline) < 0 && guard != 0u) {
        bl_usb_poll();
        guard--;
    }

    *(volatile uint32_t *)(uintptr_t)FW_BL_MAGIC_ADDR = FW_BL_MAGIC_VALUE;

    /* SYSRESETREQ. dsb/isb so the magic write is not buffered past it. */
    *(volatile uint32_t *)(uintptr_t)0xE000ED0Cu = 0x05FA0004u;
    __asm__ volatile ("dsb" ::: "memory");
    __asm__ volatile ("isb" ::: "memory");
    for (;;) {
    }
}

/* Power-on settle: FW_CART_POWERON_ACK_MS is spent before the ACK, the rest
 * here, charged to whatever touches the cartridge first. cart.h:120-171. */
static uint32_t g_cart_ready_at;

/* Push n bytes. Returns 0 if the host stopped reading, and the caller must then
 * abandon the transfer. */
static int pump(const uint8_t *p, uint32_t n)
{
    uint32_t sent = 0u;
#if FW_FAST_CLOCK
    /* Armed lazily, so a healthy transfer reads the clock zero times. */
    uint32_t deadline = 0u;
    uint8_t  armed = 0u;
#else
    uint32_t deadline = bl_time_ms() + FW_TX_STALL_MS;
#endif

    while (sent < n) {
        uint32_t k;
        bl_usb_poll();
        k = bl_usb_tx(&p[sent], n - sent);
        if (k == 0u && g_state.pump_stalls != 0xFFFFu) {
            /* VAR16_FW_PUMP_STALLS, saturating. */
            g_state.pump_stalls++;
        }
        if (k != 0u) {
            sent += k;
#if FW_FAST_CLOCK
            armed = 0u;
        } else if (armed == 0u) {
            deadline = bl_time_ms() + FW_TX_STALL_MS;
            armed = 1u;
#else
            deadline = bl_time_ms() + FW_TX_STALL_MS;
#endif
        } else if ((int32_t)(bl_time_ms() - deadline) >= 0) {
#if FW_USB_IRQ
            /* An absent host and a wedged interrupt look the same here. */
            if (bl_usb_irq_armed()) {
                bl_usb_irq_disarm(BL_USB_IRQ_FAULT_STALL);
            }
#endif
            return 0;
        }
    }
    return 1;
}

static void cart_wait_ready(void)
{
    /* Do not replace with a probe: an unready ChisFlash returns stable reads
     * shifted by one halfword, so consecutive samples agree while wrong. */
    while ((int32_t)(bl_time_ms() - g_cart_ready_at) < 0) {
        bl_usb_poll();
    }
}

void fw_cart_wait_ready(void)
{
    cart_wait_ready();
}

/* BOOTLOADER_RESET(), reachable from the LK_device header. Never returns. */
void fw_lk_bootloader_reset(void)
{
    enter_update_mode();
}

/* AGB_READ_METHOD, halfwords per latch (stock 0x612C). Host default is Single
 * (LK_Device.py:138). */
static uint32_t agb_latch_bytes(const fw_state_t *st)
{
    switch (st->agb_read_method) {
    case 0u:  return 2u;                /* Single */
    case 1u:  return 4u;                /* MemCpy */
    default:  return st->cart_latch;    /* Stream */
    }
}

static uint32_t step_bytes(const fw_state_t *st)
{
    return st->cart_step_pin ? (uint32_t)st->cart_step_pin
                             : (uint32_t)st->cart_step;
}

/* Stream must not be interrupted between its latch and its deselect, so its
 * step is the latch group; Single and MemCpy re-latch every 2 or 4 bytes. */
static uint32_t agb_step_bytes(const fw_state_t *st)
{
    if (st->cart_step_pin != 0u) {
        return st->cart_step_pin;
    }
    if (st->agb_read_method == 2u) {
        return st->cart_latch;
    }
    return st->cart_step;
}

/* Reflected 0xEDB88320, in .rodata. */
static const uint32_t crc32_tab[256] = {
    0x00000000U, 0x77073096U, 0xEE0E612CU, 0x990951BAU,
    0x076DC419U, 0x706AF48FU, 0xE963A535U, 0x9E6495A3U,
    0x0EDB8832U, 0x79DCB8A4U, 0xE0D5E91EU, 0x97D2D988U,
    0x09B64C2BU, 0x7EB17CBDU, 0xE7B82D07U, 0x90BF1D91U,
    0x1DB71064U, 0x6AB020F2U, 0xF3B97148U, 0x84BE41DEU,
    0x1ADAD47DU, 0x6DDDE4EBU, 0xF4D4B551U, 0x83D385C7U,
    0x136C9856U, 0x646BA8C0U, 0xFD62F97AU, 0x8A65C9ECU,
    0x14015C4FU, 0x63066CD9U, 0xFA0F3D63U, 0x8D080DF5U,
    0x3B6E20C8U, 0x4C69105EU, 0xD56041E4U, 0xA2677172U,
    0x3C03E4D1U, 0x4B04D447U, 0xD20D85FDU, 0xA50AB56BU,
    0x35B5A8FAU, 0x42B2986CU, 0xDBBBC9D6U, 0xACBCF940U,
    0x32D86CE3U, 0x45DF5C75U, 0xDCD60DCFU, 0xABD13D59U,
    0x26D930ACU, 0x51DE003AU, 0xC8D75180U, 0xBFD06116U,
    0x21B4F4B5U, 0x56B3C423U, 0xCFBA9599U, 0xB8BDA50FU,
    0x2802B89EU, 0x5F058808U, 0xC60CD9B2U, 0xB10BE924U,
    0x2F6F7C87U, 0x58684C11U, 0xC1611DABU, 0xB6662D3DU,
    0x76DC4190U, 0x01DB7106U, 0x98D220BCU, 0xEFD5102AU,
    0x71B18589U, 0x06B6B51FU, 0x9FBFE4A5U, 0xE8B8D433U,
    0x7807C9A2U, 0x0F00F934U, 0x9609A88EU, 0xE10E9818U,
    0x7F6A0DBBU, 0x086D3D2DU, 0x91646C97U, 0xE6635C01U,
    0x6B6B51F4U, 0x1C6C6162U, 0x856530D8U, 0xF262004EU,
    0x6C0695EDU, 0x1B01A57BU, 0x8208F4C1U, 0xF50FC457U,
    0x65B0D9C6U, 0x12B7E950U, 0x8BBEB8EAU, 0xFCB9887CU,
    0x62DD1DDFU, 0x15DA2D49U, 0x8CD37CF3U, 0xFBD44C65U,
    0x4DB26158U, 0x3AB551CEU, 0xA3BC0074U, 0xD4BB30E2U,
    0x4ADFA541U, 0x3DD895D7U, 0xA4D1C46DU, 0xD3D6F4FBU,
    0x4369E96AU, 0x346ED9FCU, 0xAD678846U, 0xDA60B8D0U,
    0x44042D73U, 0x33031DE5U, 0xAA0A4C5FU, 0xDD0D7CC9U,
    0x5005713CU, 0x270241AAU, 0xBE0B1010U, 0xC90C2086U,
    0x5768B525U, 0x206F85B3U, 0xB966D409U, 0xCE61E49FU,
    0x5EDEF90EU, 0x29D9C998U, 0xB0D09822U, 0xC7D7A8B4U,
    0x59B33D17U, 0x2EB40D81U, 0xB7BD5C3BU, 0xC0BA6CADU,
    0xEDB88320U, 0x9ABFB3B6U, 0x03B6E20CU, 0x74B1D29AU,
    0xEAD54739U, 0x9DD277AFU, 0x04DB2615U, 0x73DC1683U,
    0xE3630B12U, 0x94643B84U, 0x0D6D6A3EU, 0x7A6A5AA8U,
    0xE40ECF0BU, 0x9309FF9DU, 0x0A00AE27U, 0x7D079EB1U,
    0xF00F9344U, 0x8708A3D2U, 0x1E01F268U, 0x6906C2FEU,
    0xF762575DU, 0x806567CBU, 0x196C3671U, 0x6E6B06E7U,
    0xFED41B76U, 0x89D32BE0U, 0x10DA7A5AU, 0x67DD4ACCU,
    0xF9B9DF6FU, 0x8EBEEFF9U, 0x17B7BE43U, 0x60B08ED5U,
    0xD6D6A3E8U, 0xA1D1937EU, 0x38D8C2C4U, 0x4FDFF252U,
    0xD1BB67F1U, 0xA6BC5767U, 0x3FB506DDU, 0x48B2364BU,
    0xD80D2BDAU, 0xAF0A1B4CU, 0x36034AF6U, 0x41047A60U,
    0xDF60EFC3U, 0xA867DF55U, 0x316E8EEFU, 0x4669BE79U,
    0xCB61B38CU, 0xBC66831AU, 0x256FD2A0U, 0x5268E236U,
    0xCC0C7795U, 0xBB0B4703U, 0x220216B9U, 0x5505262FU,
    0xC5BA3BBEU, 0xB2BD0B28U, 0x2BB45A92U, 0x5CB36A04U,
    0xC2D7FFA7U, 0xB5D0CF31U, 0x2CD99E8BU, 0x5BDEAE1DU,
    0x9B64C2B0U, 0xEC63F226U, 0x756AA39CU, 0x026D930AU,
    0x9C0906A9U, 0xEB0E363FU, 0x72076785U, 0x05005713U,
    0x95BF4A82U, 0xE2B87A14U, 0x7BB12BAEU, 0x0CB61B38U,
    0x92D28E9BU, 0xE5D5BE0DU, 0x7CDCEFB7U, 0x0BDBDF21U,
    0x86D3D2D4U, 0xF1D4E242U, 0x68DDB3F8U, 0x1FDA836EU,
    0x81BE16CDU, 0xF6B9265BU, 0x6FB077E1U, 0x18B74777U,
    0x88085AE6U, 0xFF0F6A70U, 0x66063BCAU, 0x11010B5CU,
    0x8F659EFFU, 0xF862AE69U, 0x616BFFD3U, 0x166CCF45U,
    0xA00AE278U, 0xD70DD2EEU, 0x4E048354U, 0x3903B3C2U,
    0xA7672661U, 0xD06016F7U, 0x4969474DU, 0x3E6E77DBU,
    0xAED16A4AU, 0xD9D65ADCU, 0x40DF0B66U, 0x37D83BF0U,
    0xA9BCAE53U, 0xDEBB9EC5U, 0x47B2CF7FU, 0x30B5FFE9U,
    0xBDBDF21CU, 0xCABAC28AU, 0x53B39330U, 0x24B4A3A6U,
    0xBAD03605U, 0xCDD70693U, 0x54DE5729U, 0x23D967BFU,
    0xB3667A2EU, 0xC4614AB8U, 0x5D681B02U, 0x2A6F2B94U,
    0xB40BBE37U, 0xC30C8EA1U, 0x5A05DF1BU, 0x2D02EF8DU,
};

/* FW_CRC32_SLICE4: four bytes per table pass, 3 KB more .rodata. Sums must
 * match the FW_CRC32_SLICE4=0 build (tools/test_crc32.py). Slice k is
 * T[k][i] = (T[k-1][i] >> 8) ^ T[0][T[k-1][i] & 0xFF], T[0] = crc32_tab. */
#ifndef FW_CRC32_SLICE4
#define FW_CRC32_SLICE4 0
#endif

#if FW_CRC32_SLICE4
static const uint32_t crc32_tab1[256] = {
    0x00000000U, 0x191B3141U, 0x32366282U, 0x2B2D53C3U,
    0x646CC504U, 0x7D77F445U, 0x565AA786U, 0x4F4196C7U,
    0xC8D98A08U, 0xD1C2BB49U, 0xFAEFE88AU, 0xE3F4D9CBU,
    0xACB54F0CU, 0xB5AE7E4DU, 0x9E832D8EU, 0x87981CCFU,
    0x4AC21251U, 0x53D92310U, 0x78F470D3U, 0x61EF4192U,
    0x2EAED755U, 0x37B5E614U, 0x1C98B5D7U, 0x05838496U,
    0x821B9859U, 0x9B00A918U, 0xB02DFADBU, 0xA936CB9AU,
    0xE6775D5DU, 0xFF6C6C1CU, 0xD4413FDFU, 0xCD5A0E9EU,
    0x958424A2U, 0x8C9F15E3U, 0xA7B24620U, 0xBEA97761U,
    0xF1E8E1A6U, 0xE8F3D0E7U, 0xC3DE8324U, 0xDAC5B265U,
    0x5D5DAEAAU, 0x44469FEBU, 0x6F6BCC28U, 0x7670FD69U,
    0x39316BAEU, 0x202A5AEFU, 0x0B07092CU, 0x121C386DU,
    0xDF4636F3U, 0xC65D07B2U, 0xED705471U, 0xF46B6530U,
    0xBB2AF3F7U, 0xA231C2B6U, 0x891C9175U, 0x9007A034U,
    0x179FBCFBU, 0x0E848DBAU, 0x25A9DE79U, 0x3CB2EF38U,
    0x73F379FFU, 0x6AE848BEU, 0x41C51B7DU, 0x58DE2A3CU,
    0xF0794F05U, 0xE9627E44U, 0xC24F2D87U, 0xDB541CC6U,
    0x94158A01U, 0x8D0EBB40U, 0xA623E883U, 0xBF38D9C2U,
    0x38A0C50DU, 0x21BBF44CU, 0x0A96A78FU, 0x138D96CEU,
    0x5CCC0009U, 0x45D73148U, 0x6EFA628BU, 0x77E153CAU,
    0xBABB5D54U, 0xA3A06C15U, 0x888D3FD6U, 0x91960E97U,
    0xDED79850U, 0xC7CCA911U, 0xECE1FAD2U, 0xF5FACB93U,
    0x7262D75CU, 0x6B79E61DU, 0x4054B5DEU, 0x594F849FU,
    0x160E1258U, 0x0F152319U, 0x243870DAU, 0x3D23419BU,
    0x65FD6BA7U, 0x7CE65AE6U, 0x57CB0925U, 0x4ED03864U,
    0x0191AEA3U, 0x188A9FE2U, 0x33A7CC21U, 0x2ABCFD60U,
    0xAD24E1AFU, 0xB43FD0EEU, 0x9F12832DU, 0x8609B26CU,
    0xC94824ABU, 0xD05315EAU, 0xFB7E4629U, 0xE2657768U,
    0x2F3F79F6U, 0x362448B7U, 0x1D091B74U, 0x04122A35U,
    0x4B53BCF2U, 0x52488DB3U, 0x7965DE70U, 0x607EEF31U,
    0xE7E6F3FEU, 0xFEFDC2BFU, 0xD5D0917CU, 0xCCCBA03DU,
    0x838A36FAU, 0x9A9107BBU, 0xB1BC5478U, 0xA8A76539U,
    0x3B83984BU, 0x2298A90AU, 0x09B5FAC9U, 0x10AECB88U,
    0x5FEF5D4FU, 0x46F46C0EU, 0x6DD93FCDU, 0x74C20E8CU,
    0xF35A1243U, 0xEA412302U, 0xC16C70C1U, 0xD8774180U,
    0x9736D747U, 0x8E2DE606U, 0xA500B5C5U, 0xBC1B8484U,
    0x71418A1AU, 0x685ABB5BU, 0x4377E898U, 0x5A6CD9D9U,
    0x152D4F1EU, 0x0C367E5FU, 0x271B2D9CU, 0x3E001CDDU,
    0xB9980012U, 0xA0833153U, 0x8BAE6290U, 0x92B553D1U,
    0xDDF4C516U, 0xC4EFF457U, 0xEFC2A794U, 0xF6D996D5U,
    0xAE07BCE9U, 0xB71C8DA8U, 0x9C31DE6BU, 0x852AEF2AU,
    0xCA6B79EDU, 0xD37048ACU, 0xF85D1B6FU, 0xE1462A2EU,
    0x66DE36E1U, 0x7FC507A0U, 0x54E85463U, 0x4DF36522U,
    0x02B2F3E5U, 0x1BA9C2A4U, 0x30849167U, 0x299FA026U,
    0xE4C5AEB8U, 0xFDDE9FF9U, 0xD6F3CC3AU, 0xCFE8FD7BU,
    0x80A96BBCU, 0x99B25AFDU, 0xB29F093EU, 0xAB84387FU,
    0x2C1C24B0U, 0x350715F1U, 0x1E2A4632U, 0x07317773U,
    0x4870E1B4U, 0x516BD0F5U, 0x7A468336U, 0x635DB277U,
    0xCBFAD74EU, 0xD2E1E60FU, 0xF9CCB5CCU, 0xE0D7848DU,
    0xAF96124AU, 0xB68D230BU, 0x9DA070C8U, 0x84BB4189U,
    0x03235D46U, 0x1A386C07U, 0x31153FC4U, 0x280E0E85U,
    0x674F9842U, 0x7E54A903U, 0x5579FAC0U, 0x4C62CB81U,
    0x8138C51FU, 0x9823F45EU, 0xB30EA79DU, 0xAA1596DCU,
    0xE554001BU, 0xFC4F315AU, 0xD7626299U, 0xCE7953D8U,
    0x49E14F17U, 0x50FA7E56U, 0x7BD72D95U, 0x62CC1CD4U,
    0x2D8D8A13U, 0x3496BB52U, 0x1FBBE891U, 0x06A0D9D0U,
    0x5E7EF3ECU, 0x4765C2ADU, 0x6C48916EU, 0x7553A02FU,
    0x3A1236E8U, 0x230907A9U, 0x0824546AU, 0x113F652BU,
    0x96A779E4U, 0x8FBC48A5U, 0xA4911B66U, 0xBD8A2A27U,
    0xF2CBBCE0U, 0xEBD08DA1U, 0xC0FDDE62U, 0xD9E6EF23U,
    0x14BCE1BDU, 0x0DA7D0FCU, 0x268A833FU, 0x3F91B27EU,
    0x70D024B9U, 0x69CB15F8U, 0x42E6463BU, 0x5BFD777AU,
    0xDC656BB5U, 0xC57E5AF4U, 0xEE530937U, 0xF7483876U,
    0xB809AEB1U, 0xA1129FF0U, 0x8A3FCC33U, 0x9324FD72U,
};

static const uint32_t crc32_tab2[256] = {
    0x00000000U, 0x01C26A37U, 0x0384D46EU, 0x0246BE59U,
    0x0709A8DCU, 0x06CBC2EBU, 0x048D7CB2U, 0x054F1685U,
    0x0E1351B8U, 0x0FD13B8FU, 0x0D9785D6U, 0x0C55EFE1U,
    0x091AF964U, 0x08D89353U, 0x0A9E2D0AU, 0x0B5C473DU,
    0x1C26A370U, 0x1DE4C947U, 0x1FA2771EU, 0x1E601D29U,
    0x1B2F0BACU, 0x1AED619BU, 0x18ABDFC2U, 0x1969B5F5U,
    0x1235F2C8U, 0x13F798FFU, 0x11B126A6U, 0x10734C91U,
    0x153C5A14U, 0x14FE3023U, 0x16B88E7AU, 0x177AE44DU,
    0x384D46E0U, 0x398F2CD7U, 0x3BC9928EU, 0x3A0BF8B9U,
    0x3F44EE3CU, 0x3E86840BU, 0x3CC03A52U, 0x3D025065U,
    0x365E1758U, 0x379C7D6FU, 0x35DAC336U, 0x3418A901U,
    0x3157BF84U, 0x3095D5B3U, 0x32D36BEAU, 0x331101DDU,
    0x246BE590U, 0x25A98FA7U, 0x27EF31FEU, 0x262D5BC9U,
    0x23624D4CU, 0x22A0277BU, 0x20E69922U, 0x2124F315U,
    0x2A78B428U, 0x2BBADE1FU, 0x29FC6046U, 0x283E0A71U,
    0x2D711CF4U, 0x2CB376C3U, 0x2EF5C89AU, 0x2F37A2ADU,
    0x709A8DC0U, 0x7158E7F7U, 0x731E59AEU, 0x72DC3399U,
    0x7793251CU, 0x76514F2BU, 0x7417F172U, 0x75D59B45U,
    0x7E89DC78U, 0x7F4BB64FU, 0x7D0D0816U, 0x7CCF6221U,
    0x798074A4U, 0x78421E93U, 0x7A04A0CAU, 0x7BC6CAFDU,
    0x6CBC2EB0U, 0x6D7E4487U, 0x6F38FADEU, 0x6EFA90E9U,
    0x6BB5866CU, 0x6A77EC5BU, 0x68315202U, 0x69F33835U,
    0x62AF7F08U, 0x636D153FU, 0x612BAB66U, 0x60E9C151U,
    0x65A6D7D4U, 0x6464BDE3U, 0x662203BAU, 0x67E0698DU,
    0x48D7CB20U, 0x4915A117U, 0x4B531F4EU, 0x4A917579U,
    0x4FDE63FCU, 0x4E1C09CBU, 0x4C5AB792U, 0x4D98DDA5U,
    0x46C49A98U, 0x4706F0AFU, 0x45404EF6U, 0x448224C1U,
    0x41CD3244U, 0x400F5873U, 0x4249E62AU, 0x438B8C1DU,
    0x54F16850U, 0x55330267U, 0x5775BC3EU, 0x56B7D609U,
    0x53F8C08CU, 0x523AAABBU, 0x507C14E2U, 0x51BE7ED5U,
    0x5AE239E8U, 0x5B2053DFU, 0x5966ED86U, 0x58A487B1U,
    0x5DEB9134U, 0x5C29FB03U, 0x5E6F455AU, 0x5FAD2F6DU,
    0xE1351B80U, 0xE0F771B7U, 0xE2B1CFEEU, 0xE373A5D9U,
    0xE63CB35CU, 0xE7FED96BU, 0xE5B86732U, 0xE47A0D05U,
    0xEF264A38U, 0xEEE4200FU, 0xECA29E56U, 0xED60F461U,
    0xE82FE2E4U, 0xE9ED88D3U, 0xEBAB368AU, 0xEA695CBDU,
    0xFD13B8F0U, 0xFCD1D2C7U, 0xFE976C9EU, 0xFF5506A9U,
    0xFA1A102CU, 0xFBD87A1BU, 0xF99EC442U, 0xF85CAE75U,
    0xF300E948U, 0xF2C2837FU, 0xF0843D26U, 0xF1465711U,
    0xF4094194U, 0xF5CB2BA3U, 0xF78D95FAU, 0xF64FFFCDU,
    0xD9785D60U, 0xD8BA3757U, 0xDAFC890EU, 0xDB3EE339U,
    0xDE71F5BCU, 0xDFB39F8BU, 0xDDF521D2U, 0xDC374BE5U,
    0xD76B0CD8U, 0xD6A966EFU, 0xD4EFD8B6U, 0xD52DB281U,
    0xD062A404U, 0xD1A0CE33U, 0xD3E6706AU, 0xD2241A5DU,
    0xC55EFE10U, 0xC49C9427U, 0xC6DA2A7EU, 0xC7184049U,
    0xC25756CCU, 0xC3953CFBU, 0xC1D382A2U, 0xC011E895U,
    0xCB4DAFA8U, 0xCA8FC59FU, 0xC8C97BC6U, 0xC90B11F1U,
    0xCC440774U, 0xCD866D43U, 0xCFC0D31AU, 0xCE02B92DU,
    0x91AF9640U, 0x906DFC77U, 0x922B422EU, 0x93E92819U,
    0x96A63E9CU, 0x976454ABU, 0x9522EAF2U, 0x94E080C5U,
    0x9FBCC7F8U, 0x9E7EADCFU, 0x9C381396U, 0x9DFA79A1U,
    0x98B56F24U, 0x99770513U, 0x9B31BB4AU, 0x9AF3D17DU,
    0x8D893530U, 0x8C4B5F07U, 0x8E0DE15EU, 0x8FCF8B69U,
    0x8A809DECU, 0x8B42F7DBU, 0x89044982U, 0x88C623B5U,
    0x839A6488U, 0x82580EBFU, 0x801EB0E6U, 0x81DCDAD1U,
    0x8493CC54U, 0x8551A663U, 0x8717183AU, 0x86D5720DU,
    0xA9E2D0A0U, 0xA820BA97U, 0xAA6604CEU, 0xABA46EF9U,
    0xAEEB787CU, 0xAF29124BU, 0xAD6FAC12U, 0xACADC625U,
    0xA7F18118U, 0xA633EB2FU, 0xA4755576U, 0xA5B73F41U,
    0xA0F829C4U, 0xA13A43F3U, 0xA37CFDAAU, 0xA2BE979DU,
    0xB5C473D0U, 0xB40619E7U, 0xB640A7BEU, 0xB782CD89U,
    0xB2CDDB0CU, 0xB30FB13BU, 0xB1490F62U, 0xB08B6555U,
    0xBBD72268U, 0xBA15485FU, 0xB853F606U, 0xB9919C31U,
    0xBCDE8AB4U, 0xBD1CE083U, 0xBF5A5EDAU, 0xBE9834EDU,
};

static const uint32_t crc32_tab3[256] = {
    0x00000000U, 0xB8BC6765U, 0xAA09C88BU, 0x12B5AFEEU,
    0x8F629757U, 0x37DEF032U, 0x256B5FDCU, 0x9DD738B9U,
    0xC5B428EFU, 0x7D084F8AU, 0x6FBDE064U, 0xD7018701U,
    0x4AD6BFB8U, 0xF26AD8DDU, 0xE0DF7733U, 0x58631056U,
    0x5019579FU, 0xE8A530FAU, 0xFA109F14U, 0x42ACF871U,
    0xDF7BC0C8U, 0x67C7A7ADU, 0x75720843U, 0xCDCE6F26U,
    0x95AD7F70U, 0x2D111815U, 0x3FA4B7FBU, 0x8718D09EU,
    0x1ACFE827U, 0xA2738F42U, 0xB0C620ACU, 0x087A47C9U,
    0xA032AF3EU, 0x188EC85BU, 0x0A3B67B5U, 0xB28700D0U,
    0x2F503869U, 0x97EC5F0CU, 0x8559F0E2U, 0x3DE59787U,
    0x658687D1U, 0xDD3AE0B4U, 0xCF8F4F5AU, 0x7733283FU,
    0xEAE41086U, 0x525877E3U, 0x40EDD80DU, 0xF851BF68U,
    0xF02BF8A1U, 0x48979FC4U, 0x5A22302AU, 0xE29E574FU,
    0x7F496FF6U, 0xC7F50893U, 0xD540A77DU, 0x6DFCC018U,
    0x359FD04EU, 0x8D23B72BU, 0x9F9618C5U, 0x272A7FA0U,
    0xBAFD4719U, 0x0241207CU, 0x10F48F92U, 0xA848E8F7U,
    0x9B14583DU, 0x23A83F58U, 0x311D90B6U, 0x89A1F7D3U,
    0x1476CF6AU, 0xACCAA80FU, 0xBE7F07E1U, 0x06C36084U,
    0x5EA070D2U, 0xE61C17B7U, 0xF4A9B859U, 0x4C15DF3CU,
    0xD1C2E785U, 0x697E80E0U, 0x7BCB2F0EU, 0xC377486BU,
    0xCB0D0FA2U, 0x73B168C7U, 0x6104C729U, 0xD9B8A04CU,
    0x446F98F5U, 0xFCD3FF90U, 0xEE66507EU, 0x56DA371BU,
    0x0EB9274DU, 0xB6054028U, 0xA4B0EFC6U, 0x1C0C88A3U,
    0x81DBB01AU, 0x3967D77FU, 0x2BD27891U, 0x936E1FF4U,
    0x3B26F703U, 0x839A9066U, 0x912F3F88U, 0x299358EDU,
    0xB4446054U, 0x0CF80731U, 0x1E4DA8DFU, 0xA6F1CFBAU,
    0xFE92DFECU, 0x462EB889U, 0x549B1767U, 0xEC277002U,
    0x71F048BBU, 0xC94C2FDEU, 0xDBF98030U, 0x6345E755U,
    0x6B3FA09CU, 0xD383C7F9U, 0xC1366817U, 0x798A0F72U,
    0xE45D37CBU, 0x5CE150AEU, 0x4E54FF40U, 0xF6E89825U,
    0xAE8B8873U, 0x1637EF16U, 0x048240F8U, 0xBC3E279DU,
    0x21E91F24U, 0x99557841U, 0x8BE0D7AFU, 0x335CB0CAU,
    0xED59B63BU, 0x55E5D15EU, 0x47507EB0U, 0xFFEC19D5U,
    0x623B216CU, 0xDA874609U, 0xC832E9E7U, 0x708E8E82U,
    0x28ED9ED4U, 0x9051F9B1U, 0x82E4565FU, 0x3A58313AU,
    0xA78F0983U, 0x1F336EE6U, 0x0D86C108U, 0xB53AA66DU,
    0xBD40E1A4U, 0x05FC86C1U, 0x1749292FU, 0xAFF54E4AU,
    0x322276F3U, 0x8A9E1196U, 0x982BBE78U, 0x2097D91DU,
    0x78F4C94BU, 0xC048AE2EU, 0xD2FD01C0U, 0x6A4166A5U,
    0xF7965E1CU, 0x4F2A3979U, 0x5D9F9697U, 0xE523F1F2U,
    0x4D6B1905U, 0xF5D77E60U, 0xE762D18EU, 0x5FDEB6EBU,
    0xC2098E52U, 0x7AB5E937U, 0x680046D9U, 0xD0BC21BCU,
    0x88DF31EAU, 0x3063568FU, 0x22D6F961U, 0x9A6A9E04U,
    0x07BDA6BDU, 0xBF01C1D8U, 0xADB46E36U, 0x15080953U,
    0x1D724E9AU, 0xA5CE29FFU, 0xB77B8611U, 0x0FC7E174U,
    0x9210D9CDU, 0x2AACBEA8U, 0x38191146U, 0x80A57623U,
    0xD8C66675U, 0x607A0110U, 0x72CFAEFEU, 0xCA73C99BU,
    0x57A4F122U, 0xEF189647U, 0xFDAD39A9U, 0x45115ECCU,
    0x764DEE06U, 0xCEF18963U, 0xDC44268DU, 0x64F841E8U,
    0xF92F7951U, 0x41931E34U, 0x5326B1DAU, 0xEB9AD6BFU,
    0xB3F9C6E9U, 0x0B45A18CU, 0x19F00E62U, 0xA14C6907U,
    0x3C9B51BEU, 0x842736DBU, 0x96929935U, 0x2E2EFE50U,
    0x2654B999U, 0x9EE8DEFCU, 0x8C5D7112U, 0x34E11677U,
    0xA9362ECEU, 0x118A49ABU, 0x033FE645U, 0xBB838120U,
    0xE3E09176U, 0x5B5CF613U, 0x49E959FDU, 0xF1553E98U,
    0x6C820621U, 0xD43E6144U, 0xC68BCEAAU, 0x7E37A9CFU,
    0xD67F4138U, 0x6EC3265DU, 0x7C7689B3U, 0xC4CAEED6U,
    0x591DD66FU, 0xE1A1B10AU, 0xF3141EE4U, 0x4BA87981U,
    0x13CB69D7U, 0xAB770EB2U, 0xB9C2A15CU, 0x017EC639U,
    0x9CA9FE80U, 0x241599E5U, 0x36A0360BU, 0x8E1C516EU,
    0x866616A7U, 0x3EDA71C2U, 0x2C6FDE2CU, 0x94D3B949U,
    0x090481F0U, 0xB1B8E695U, 0xA30D497BU, 0x1BB12E1EU,
    0x43D23E48U, 0xFB6E592DU, 0xE9DBF6C3U, 0x516791A6U,
    0xCCB0A91FU, 0x740CCE7AU, 0x66B96194U, 0xDE0506F1U,
};

static uint32_t crc32_step(uint32_t crc, const uint8_t *p, uint32_t n)
{
    /* The protocol permits an odd pointer or length, so keep the byte paths. */
    if ((((uintptr_t)p) & 3u) == 0u) {
        const uint32_t *w = (const uint32_t *)(const void *)p;
        const uint32_t *t0 = crc32_tab,  *t1 = crc32_tab1;
        const uint32_t *t2 = crc32_tab2, *t3 = crc32_tab3;
        uint32_t k = n >> 2;

        n &= 3u;
        while (k--) {
            uint32_t c = crc ^ *w++;
            uint32_t a = t3[c & 0xFFu]         ^ t2[(c >> 8) & 0xFFu];
            uint32_t b = t1[(c >> 16) & 0xFFu] ^ t0[c >> 24];
            crc = a ^ b;
        }
        p = (const uint8_t *)(const void *)w;
    }
    while (n--) {
        crc = crc32_tab[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}
#else
static uint32_t crc32_step(uint32_t crc, const uint8_t *p, uint32_t n)
{
    while (n--) {
        crc = crc32_tab[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}
#endif

/* Chunked through g_reply so the requested length is not bounded by its size. */
static void do_crc32(fw_state_t *st, uint8_t *out)
{
    cart_wait_ready();
    uint32_t left = st->crc_len;
    uint32_t addr = st->address;
    uint32_t crc = 0xFFFFFFFFu;

    st->crc_requested = 0u;

    if (st->mode == FW_MODE_DMG) {
        fw_cart_dmg_setup();
    }
    while (left) {
        uint32_t want = (left > sizeof(g_reply)) ? (uint32_t)sizeof(g_reply)
                                                 : left;
        if (st->mode == FW_MODE_DMG) {
            (void)fw_cart_dmg_read(addr, g_reply, want,
                                   st->dmg_read_method, st->dmg_read_cs_pulse);
            addr += want;
        } else {
#if !FW_AGB_LEAF
            uint32_t off = 0u;
#endif
            /* Always Stream: CompareCRC32 hashes 0x20000 bytes under a 1 s
             * timeout (LK_Device.py:132, :2254). */
            uint32_t latch = st->cart_latch;
#if FW_AGB_LEAF
            /* Counts are halfwords, not bytes, and both must be non-zero. */
            if (want != 0u) {
                fw_cart_agb_read_leaf(addr, (uint16_t *)(void *)&g_reply[0],
                                      want / 2u, latch / 2u);
            }
#else
            while (off < want) {
                uint32_t grp = want - off;
                if (grp > latch) {
                    grp = latch;
                }
                fw_cart_agb_open(addr + (off / 2u));
                (void)fw_cart_agb_burst(&g_reply[off], grp);
                fw_cart_agb_close();
                off += grp;
            }
#endif
            addr += want / 2u;
        }
        crc = crc32_step(crc, g_reply, want);
        left -= want;
        bl_usb_poll();          /* a long CRC must not starve the endpoint */
    }
    crc ^= 0xFFFFFFFFu;

    out[0] = (uint8_t)(crc >> 24);
    out[1] = (uint8_t)(crc >> 16);
    out[2] = (uint8_t)(crc >> 8);
    out[3] = (uint8_t)crc;
    /* Four bytes and no ACK: CompareCRC32 does _read(4) and nothing else
     * (LK_Device.py:2269-2271). A fifth byte desynchronises the pipe. */
}

/* One 64-bit word per frame, address included (stock sub_64B8). The first frame
 * after an idle bus is unreliable. */
static void do_eeprom_read(fw_state_t *st)
{
    uint32_t len = st->transfer_size;
    uint32_t off = 0u;

    cart_wait_ready();
    fw_cart_agb_eeprom_bus();
    while (off + 8u <= len) {
        fw_cart_agb_eeprom_read(st->address + (off / 8u), &g_reply[off],
                                st->eeprom_sel);
        off += 8u;
    }
    while (off < len) {
        g_reply[off++] = 0xFFu;         /* the host is committed to len bytes */
    }
    if (!pump(g_reply, len)) {
        return;
    }
    st->address += len / 8u;            /* the host sets it once and loops */
}

/* AGB save memory is behind /CS2, its data on the A16..A23 pins, so the bus is
 * turned around per run: fw_cart_agb_sram_open/close, stock sub_A5A4(1)/(2). */

/* FW_SAVE_TX_DIRECT: nominate g_reply rather than copy through the staging
 * ring, arming a CDC ZLP when it empties mid-command. Publish after fill. */
#ifndef FW_SAVE_TX_DIRECT
#define FW_SAVE_TX_DIRECT 0
#endif

#if FW_SAVE_TX_DIRECT && !FW_TX_DIRECT
#error "FW_SAVE_TX_DIRECT needs FW_TX_DIRECT: it nominates a direct region."
#endif

static void do_save_read(fw_state_t *st)
{
    cart_wait_ready();
    uint32_t len = st->transfer_size;
    uint32_t step = step_bytes(st);
    uint32_t off = 0u;

#if FW_SAVE_TX_DIRECT
    __extension__ _Static_assert(FW_MAX_TRANSFER <= 0xFFFFu,
                                 "do_save_read: a direct transfer is indexed "
                                 "by a uint16_t");
    bl_usb_tx_direct_begin(g_reply, (uint16_t)len);
#endif

    fw_cart_agb_sram_open();
    while (off < len) {
        uint32_t want = (len - off > step) ? step : (len - off);
        uint8_t *p = &g_reply[off];
        (void)fw_cart_agb_sram_read(st->address + off, p, want);
#if FW_SAVE_TX_DIRECT
        off += want;
        bl_usb_tx_direct_publish((uint16_t)off);
        /* One poll per packet: bl_usb_poll() services at most one IN. */
        {
            uint32_t k = want;
            while (k != 0u) {
                bl_usb_poll();
                k = (k > 64u) ? (k - 64u) : 0u;
            }
        }
#else
        if (!pump(p, want)) {
            fw_cart_agb_sram_close();
            return;
        }
        off += want;
#endif
    }
    fw_cart_agb_sram_close();

#if FW_SAVE_TX_DIRECT
    /* Must run to the end: withdrawing the region with packets still owed
     * loses the last 64 bytes. */
    {
        uint32_t deadline = 0u;
        uint8_t  armed = 0u;
        uint16_t seen = bl_usb_tx_direct_sent();

        while (bl_usb_tx_direct_sent() < (uint16_t)len) {
            uint16_t now_sent;

            bl_usb_poll();
            now_sent = bl_usb_tx_direct_sent();
            if (now_sent != seen) {
                seen  = now_sent;
                armed = 0u;
            } else if (armed == 0u) {
                deadline = bl_time_ms() + FW_TX_STALL_MS;
                armed = 1u;
            } else if ((int32_t)(bl_time_ms() - deadline) >= 0) {
                if (st->pump_stalls != 0xFFFFu) {
                    st->pump_stalls++;
                }
                bl_usb_tx_direct_end();
                return;
            }
        }
    }
    bl_usb_tx_direct_end();
#endif

    /* The host sets ADDRESS once and loops (LK_Device.py:1740-1746). */
    st->address += len;
}

/* Already ACKed when this runs; payload_done() answers off the wire. */
static void do_save_write(fw_state_t *st)
{
    cart_wait_ready();
    const uint8_t *data = fw_proto_payload();
    uint32_t len = st->save_write_len;
    uint32_t i;

    st->save_write_requested = 0u;

    if (st->save_write_is_dmg) {
        /* DMG save RAM is the ordinary bus, /CS per DMG_WRITE_CS_PULSE. */
        for (i = 0; i < len; i++) {
            fw_cart_dmg_write(st->address + i, data[i], st->dmg_write_cs_pulse);
        }
        st->address += len;
        return;
    }

    if (st->save_write_eeprom) {
        uint32_t w;
        fw_cart_agb_eeprom_bus();
        for (w = 0; w + 8u <= len; w += 8u) {
            fw_cart_agb_eeprom_write(st->address + (w / 8u), &data[w],
                                     st->save_write_eeprom);
        }
        st->address += len / 8u;
        return;
    }

    if (st->save_write_flash) {
        fw_cart_agb_sram_program(st->address, data, len, st->save_write_flash);
        /* Method 2 counts in 128-byte sectors, method 1 in bytes
         * (LK_Device.py:3651; stock 0x67B8 per sector, 0x6830 per byte). */
        st->address += (st->save_write_flash == 2u) ? (len >> 7) : len;
        return;
    }

    fw_cart_agb_sram_open();
    for (i = 0; i < len; i++) {
        fw_cart_agb_sram_write(st->address + i, data[i]);
    }
    fw_cart_agb_sram_close();
    st->address += len;
}

#ifndef FW_DMG_TX_DIRECT
#define FW_DMG_TX_DIRECT 0
#endif

#if FW_DMG_TX_DIRECT && !FW_TX_DIRECT
#error "FW_DMG_TX_DIRECT needs FW_TX_DIRECT: it nominates a direct region."
#endif

#ifndef FW_M3D_TX_DIRECT
#define FW_M3D_TX_DIRECT 0
#endif

#if FW_M3D_TX_DIRECT && !FW_TX_DIRECT
#error "FW_M3D_TX_DIRECT needs FW_TX_DIRECT: it nominates a direct region."
#endif

#if FW_M3D_TX_DIRECT
/* Publish as the page fills, so the wire runs while the bus does. Counts are
 * halfword-granular: fw_cart_agb_3d_read masks count to even (cart.c:1644), so
 * an odd sub-chunk would drop a byte and desynchronise the publish. */
static void m3d_read_overlapped(fw_state_t *st)
{
    uint32_t len  = st->transfer_size;
    uint32_t step = step_bytes(st);
    uint32_t off  = 0u;

    bl_usb_tx_direct_begin(g_reply, (uint16_t)len);

    while (off < len) {
        uint32_t want = (len - off > step) ? step : (len - off);
        want &= ~1u;
        if (want == 0u) {
            break;                      /* odd tail: the reader would drop it */
        }
        (void)fw_cart_agb_3d_read(&g_reply[off], want);
        off += want;
        bl_usb_tx_direct_publish((uint16_t)off);
        bl_usb_poll();
    }

    {
        uint32_t deadline = 0u;
        uint8_t  armed = 0u;
        uint16_t seen = bl_usb_tx_direct_sent();

        while (bl_usb_tx_direct_sent() < (uint16_t)off) {
            uint16_t now_sent;

            bl_usb_poll();
            now_sent = bl_usb_tx_direct_sent();
            if (now_sent != seen) {
                seen  = now_sent;
                armed = 0u;
            } else if (armed == 0u) {
                deadline = bl_time_ms() + FW_TX_STALL_MS;
                armed = 1u;
            } else if ((int32_t)(bl_time_ms() - deadline) >= 0) {
                if (st->pump_stalls != 0xFFFFu) {
                    st->pump_stalls++;
                }
                bl_usb_tx_direct_end();
                return;
            }
        }
    }
    bl_usb_tx_direct_end();
}
#endif

static void do_dmg_read(fw_state_t *st)
{
    cart_wait_ready();
    uint32_t len = st->transfer_size;
    uint32_t step = step_bytes(st);
    uint32_t off = 0u;

#if FW_DMG_TX_DIRECT
    /* Nominate g_reply rather than copy through the staging ring. */
    __extension__ _Static_assert(FW_MAX_TRANSFER <= 0xFFFFu,
                                 "do_dmg_read: a direct transfer is indexed "
                                 "by a uint16_t");
    bl_usb_tx_direct_begin(g_reply, (uint16_t)len);
#endif

    fw_cart_dmg_setup();

    while (off < len) {
        uint32_t want = (len - off > step) ? step : (len - off);
        uint8_t *p = &g_reply[off];

#if FW_DMG_READ_PUB && FW_DMG_TX_DIRECT
        /* Publish between sub-chunks, never inside fw_cart_dmg_read: an opaque
         * call in its loop costs it every register. */
        {
            uint32_t sub = 0u;
            while (sub < want) {
                uint32_t g = (want - sub > FW_DMG_PUB_GRAIN)
                             ? (uint32_t)FW_DMG_PUB_GRAIN : (want - sub);
#if FW_DMG_PROFILE
                uint32_t prof_r0 = prof_now();
#endif
                (void)fw_cart_dmg_read(st->address + off + sub, p + sub, g,
                                       st->dmg_read_method,
                                       st->dmg_read_cs_pulse);
#if FW_DMG_PROFILE
                g_prof_cyc_rd_bus += prof_delta(prof_r0);
                g_prof_rd_bytes += g;
                prof_r0 = prof_now();
#endif
                sub += g;
                bl_usb_tx_direct_publish((uint16_t)(off + sub));
#if FW_DMG_PROFILE
                g_prof_cyc_rd_pub += prof_delta(prof_r0);
#endif
            }
        }
#else
        (void)fw_cart_dmg_read(st->address + off, p, want,
                               st->dmg_read_method, st->dmg_read_cs_pulse);
#endif

#if FW_DMG_TX_DIRECT
        /* Publish only bytes the cartridge has already written. */
        off += want;
        bl_usb_tx_direct_publish((uint16_t)off);
#if FW_DMG_TX_OVERLAP
        /* Do not drain the chunk here; that serialises bus and wire. This poll
         * only primes an idle endpoint. */
        bl_usb_poll();
#else
        {
            /* One poll per packet: bl_usb_poll() services at most one IN. */
            uint32_t k = want;
            while (k != 0u) {
                bl_usb_poll();
                k = (k > 64u) ? (k - 64u) : 0u;
            }
        }
#endif
#else
        if (!pump(p, want)) {
            return;
        }
        off += want;
#endif
    }

#if FW_DMG_TX_DIRECT
    /* Must run to the end: withdrawing the region with packets still owed
     * loses the last 64 bytes. */
    {
        uint32_t deadline = 0u;
        uint8_t  armed = 0u;
        uint16_t seen = bl_usb_tx_direct_sent();

        while (bl_usb_tx_direct_sent() < (uint16_t)len) {
            uint16_t now_sent;

            bl_usb_poll();
            now_sent = bl_usb_tx_direct_sent();
            if (now_sent != seen) {
                seen  = now_sent;
                armed = 0u;
            } else if (armed == 0u) {
                deadline = bl_time_ms() + FW_TX_STALL_MS;
                armed = 1u;
            } else if ((int32_t)(bl_time_ms() - deadline) >= 0) {
                if (st->pump_stalls != 0xFFFFu) {
                    st->pump_stalls++;
                }
                bl_usb_tx_direct_end();
                return;
            }
        }
    }
    bl_usb_tx_direct_end();
#endif

    /* DMG advances by bytes, not halfwords. */
    st->address += len;
}

/* Stock's sub_7014. The done test differs by command set and fails silently
 * when wrong: AMD compares the word read back (DQ7 reads back inverted while in
 * flight), Intel and Sharp read status, Sharp only after a 0x70 write. */
#ifndef FW_STATUS_WAIT_TIGHT
#define FW_STATUS_WAIT_TIGHT 0
#endif

static uint32_t agb_status_wait(fw_state_t *st, uint32_t hwaddr, uint16_t want)
{
    uint32_t deadline = bl_time_ms() + 500u;
    uint32_t amd = (st->flash_command_set == 1u);

    for (;;) {
        uint16_t v;

        if (st->flash_sharp_verify_sr) {
            fw_cart_agb_write(hwaddr, 0x70u);
            fw_cart_delay_nops(56u);    /* stock 0x7036-0x7070, counted */
        }
        v = fw_cart_agb_peek(hwaddr);
        if (amd ? (v == want)
                : ((v & st->status_reg_mask)
                   == (st->status_reg_value & st->status_reg_mask))) {
            return 1u;
        }
        if ((int32_t)(bl_time_ms() - deadline) >= 0) {
            st->status_register = v;
            return 0u;
        }
        bl_usb_poll();

#if FW_STATUS_WAIT_TIGHT
        /* Not on the sharp path: it needs its own 0x70 first. */
        if (!st->flash_sharp_verify_sr) {
            v = fw_cart_agb_peek(hwaddr);
            if (amd ? (v == want)
                    : ((v & st->status_reg_mask)
                       == (st->status_reg_value & st->status_reg_mask))) {
                return 1u;
            }
        }
#endif
    }
}

/* Program one flash buffer. `done' is a byte offset into the block, `have' a
 * word count; returns 0 on timeout. Streamed and trailing chunks share it. */
static uint32_t agb_program_chunk(fw_state_t *st, const uint8_t *data,
                                  uint32_t done, uint32_t have)
{
    uint32_t sa = st->address + (done / 2u);
    uint32_t intel = (st->flash_command_set != 1u);
    uint32_t lastoff;
    uint16_t want;
    uint32_t ok;
    uint32_t w;

#if FW_AGB_SKIP_FF
    /* Programming NOR only clears bits, so a buffer of all ones writes nothing
     * to any cell, erased or not. The host skips whole all-0xFF 1024-byte
     * blocks; the 64-byte chunks inside a mixed block still cost a full unlock,
     * burst, confirm and buffer-program wait each. data[done] is 4-aligned:
     * P.pay is word-aligned and done is a multiple of buffer_size. */
    {
        const uint32_t *p32 = (const uint32_t *)(const void *)&data[done];
        uint32_t nw = ((uint32_t)have * 2u) / 4u;
        uint32_t acc = 0xFFFFFFFFu;
        for (w = 0; w < nw; w++) {
            acc &= p32[w];
        }
        if (acc == 0xFFFFFFFFu && (((uint32_t)have * 2u) & 3u) == 0u) {
            return 1u;
        }
    }
#endif

    if (intel) {
        fw_cart_agb_write(sa, st->flash_cmd_val[0]);   /* 0xE8 */
        fw_cart_agb_write(sa, (uint16_t)(have - 1u));
    } else {
        fw_cart_agb_write(st->flash_cmd_addr[0], st->flash_cmd_val[0]);
        fw_cart_agb_write(st->flash_cmd_addr[1], st->flash_cmd_val[1]);
        fw_cart_agb_write(sa, st->flash_cmd_val[2]);
        /* Two 8-bit dies on the 16-bit bus, so the count goes in both halves.
         * Profiles pre-double their own literals. LK.c:1907-1911. */
        if (st->flash_double_die) {
            fw_cart_agb_write(sa, (uint16_t)(((have - 1u) << 8) | (have - 1u)));
        } else {
            fw_cart_agb_write(sa, (uint16_t)(have - 1u));
        }
    }
#if FW_AGB_WRITE_BURST
    /* Stock inlines this loop at 0x7794-0x780C with the direction writes
     * hoisted; the burst reproduces its intervals. */
    (void)w;
    fw_cart_agb_write_burst(sa, &data[done], have);
#else
    for (w = 0; w < have; w++) {
        uint32_t o = done + w * 2u;
        fw_cart_agb_write(sa + w, (uint16_t)(data[o] | (data[o + 1u] << 8)));
    }
#endif
    if (intel) {
        fw_cart_agb_write(sa, st->flash_cmd_val[3]);   /* 0xD0 */
    } else {
        fw_cart_agb_write(sa, st->flash_cmd_val[5]);
    }

    /* An AMD buffered program retires as a unit, so poll the last word of the
     * buffer. LK.c:1953. */
    lastoff = done + (have - 1u) * 2u;
    want = (uint16_t)(data[lastoff] | (data[lastoff + 1u] << 8));
    ok = agb_status_wait(st, sa + (have - 1u), want);

    if (intel) {
        /* Read-array; stock issues it after the confirm's poll (0x7A1A). */
        fw_cart_agb_write(sa, st->flash_cmd_val[4]);  /* 0xFF */
    }
    return ok;
}

/* Program what has arrived while the rest arrives. AGB buffered only; method 1
 * and DMG fall through to do_flash_program(). */
#ifndef FW_WRITE_STREAM
#define FW_WRITE_STREAM 1
#endif

static void agb_stream_pump(fw_state_t *st)
{
#if FW_WRITE_STREAM
    uint32_t filled = fw_proto_payload_filled();
    uint32_t chunk;

    if (filled == 0u) {
        /* Do not clear program_done here: it would wrap `filled - program_done'
         * and run the loop past the payload. fw_proto_feed() owns the clear. */
        return;
    }
    if (st->mode != FW_MODE_AGB || st->flash_method != 2u
        || st->buffer_size < 2u) {
        return;
    }
    /* Already timed out: the part is in write-buffer-abort and every further
     * buffer costs 500 ms. */
    if (st->program_stream_err) {
        return;
    }

    /* Whole words, matching do_flash_program()'s clamp, or the streamed and
     * trailing chunks disagree on where the next one starts. */
    chunk = (uint32_t)(st->buffer_size / 2u) * 2u;

    /* Written to avoid an unsigned wrap: this loop drives cartridge writes. */
    while (filled >= (uint32_t)st->program_done + chunk) {
        if (st->program_done == 0u) {
            /* Clear here, not on the way out: the host reads it much later. */
            st->program_streamed = 0u;
        }
        bl_usb_poll();
        if (!agb_program_chunk(st, fw_proto_payload(),
                               st->program_done, chunk / 2u)) {
            /* One ack per block, so a timed-out chunk is carried forward. */
            st->program_stream_err = 1u;
            break;
        }
        st->program_done = (uint16_t)(st->program_done + chunk);
        st->program_streamed++;
    }
#else
    (void)st;   /* serialised build: do_flash_program() does all of it */
#endif
}

/* The DMG completion poll. AMD data-polls (LK.c:1196-1221); Intel and Sharp use
 * the host's mask/value (:1222-1245). No 0x70 pre-write here, unlike AGB. */

#ifndef FW_DMG_POLL_STRIDE
#define FW_DMG_POLL_STRIDE 1
#endif

static uint32_t dmg_status_wait(fw_state_t *st, uint32_t addr, uint8_t want)
{
    uint32_t deadline = bl_time_ms() + 500u;
    uint32_t amd = (st->flash_command_set == 1u);
/* cart.c defines fw_cart_dmg_status_poll_* only under FW_DMG_WRITE_BURST, so
 * without it this arm does not link. The loop below is the fallback. */
#if FW_DMG_POLL_TIGHT && FW_DMG_WRITE_BURST
    /* The turnaround is hoisted out of the loop; the /RD-low to sample window
     * keeps its 8 nops, or an early sample calls a write finished. */
    if (amd) {
        uint32_t tick = 0u;

        fw_cart_dmg_status_poll_open();
        for (;;) {
            uint8_t v = fw_cart_dmg_status_poll_read(addr);
#if FW_DMG_PROFILE
            g_prof_poll_iters++;
#endif
            if (v == want) {
                fw_cart_dmg_status_poll_close();
                return 1u;
            }
            /* bl_time_ms() converts SysTick cycles to milliseconds and is the
             * bulk of an iteration. The deadline is 500 ms; reading it once per
             * FW_DMG_POLL_STRIDE samples cannot overrun it meaningfully. */
            tick++;
            if (tick >= (uint32_t)FW_DMG_POLL_STRIDE) {
                tick = 0u;
                if ((int32_t)(bl_time_ms() - deadline) >= 0) {
                    fw_cart_dmg_status_poll_close();
                    st->status_register = v;
                    return 0u;
                }
            }
            bl_usb_poll();
        }
    }
#endif

    for (;;) {
        uint8_t v = 0u;

        /* Method 0, the plain /RD cycle: DMG_READ_METHOD is about ROM dumps,
         * not status polls. LK.c:1206-1209. */
        (void)fw_cart_dmg_read(addr, &v, 1u, 0u, 0u);
        if (amd ? (v == want)
                : (((uint16_t)v & st->status_reg_mask)
                   == (st->status_reg_value & st->status_reg_mask))) {
            return 1u;
        }
        if ((int32_t)(bl_time_ms() - deadline) >= 0) {
            st->status_register = v;
            return 0u;
        }
        bl_usb_poll();
    }
}

/* Replay the 0xB8 bank-change table. type 0: the stored u32 is the address and
 * the bank is the data; type 1: the stored u32 is the data and the bank is the
 * address. Count 0 means write the bank to 0x2000. LK.c:1285-1305. */
static void dmg_apply_bank_change(fw_state_t *st, uint16_t index)
{
    uint32_t k;

    if (st->dmg_bank_cmd_count == 0u) {
        fw_cart_dmg_write(0x2000u, (uint8_t)index, st->dmg_write_cs_pulse);
        return;
    }
    for (k = 0; k < st->dmg_bank_cmd_count; k++) {
        if (st->dmg_bank_cmd_type[k] == 0u) {
            fw_cart_dmg_write(st->dmg_bank_cmd_val[k], (uint8_t)index,
                              st->dmg_write_cs_pulse);
        } else {
            fw_cart_dmg_write(index, (uint8_t)st->dmg_bank_cmd_val[k],
                              st->dmg_write_cs_pulse);
        }
    }
}

/* FLASH_METHOD 1 on DMG: one unlock and one poll per byte, LK.c:1806-1877.
 * flash_commands_on_bank_1 unlocks inside the switchable 0x4000-0x7FFF window
 * (LK.c:1810-1818). Intel's PA and PD arrive as literal zero, which on DMG is
 * the MBC RAM-enable register (LK.c:1854-1858). */
static uint32_t dmg_program_unbuffered(fw_state_t *st, const uint8_t *data,
                                       uint32_t n)
{
    uint32_t amd = (st->flash_command_set == 1u);
    uint32_t ok = 1u;
    uint32_t i;
#if FW_DMG_WRITE_BURST
    const uint32_t burst_ok = (!st->flash_commands_bank_1
                               && !st->dmg_write_cs_pulse
                               && !st->flash_pulse_reset
                               && g_dmg_we_is_wr()) ? 1u : 0u;
    const uint32_t *cmd_a = st->flash_cmd_addr;
    const uint16_t *cmd_v = st->flash_cmd_val;
#endif
#if FW_DMG_UNLOCK_BYPASS && FW_DMG_WRITE_BURST
    /* Entered once for the whole payload. The host erases through its own
     * command, so no erase lands between the enter and the exit below. */
    const uint32_t bypass = (amd && burst_ok) ? 1u : 0u;

    if (bypass) {
        fw_cart_dmg_amd_bypass_enter(cmd_a, cmd_v);
    }
#endif

    for (i = 0; i < n; i++) {
        uint32_t addr = st->address + i;

#if FW_DMG_SKIP_FF
        /* NOR only clears bits, so 0xFF is a no-op on any current value. */
        if (data[i] == 0xFFu) {
            continue;
        }
#endif

        if (amd) {
            if (st->flash_commands_bank_1) {
                dmg_apply_bank_change(st, 1u);
            }
#if FW_DMG_UNLOCK_BYPASS && FW_DMG_WRITE_BURST
            if (bypass) {
                fw_cart_dmg_amd_bypass_byte(cmd_v[2], addr, data[i]);
            } else
#endif
#if FW_DMG_WRITE_BURST
            /* Plain /WR, no /CS pulse: the bank-1 variant needs its bank change
             * between the third command and the data write. */
            if (burst_ok) {
                fw_cart_dmg_amd_program_byte(cmd_a, cmd_v, addr, data[i]);
            } else
#endif
            {
            fw_cart_dmg_flash_write(st->flash_cmd_addr[0],
                                    (uint8_t)st->flash_cmd_val[0],
                                    st->dmg_write_cs_pulse);    /* AAA=AA */
            fw_cart_dmg_flash_write(st->flash_cmd_addr[1],
                                    (uint8_t)st->flash_cmd_val[1],
                                    st->dmg_write_cs_pulse);    /* 555=55 */
            fw_cart_dmg_flash_write(st->flash_cmd_addr[2],
                                    (uint8_t)st->flash_cmd_val[2],
                                    st->dmg_write_cs_pulse);    /* AAA=A0 */
            if (st->flash_commands_bank_1) {
                dmg_apply_bank_change(st, st->last_bank_accessed);
            }
            fw_cart_dmg_flash_write(addr, data[i],
                                    st->dmg_write_cs_pulse);    /* PA=PD  */
            }

            /* /RESET puts the mapper back to bank 1, so re-select afterwards.
             * LK.c:1820-1829. */
            if (st->flash_pulse_reset) {
                fw_cart_dmg_pulse_reset();
                if (st->dmg_rom_bank != 0u) {
                    dmg_apply_bank_change(st, st->last_bank_accessed);
                }
            }
            if (!dmg_status_wait(st, addr, data[i])) {
                ok = 0u;
                break;
            }
        } else {
            fw_cart_dmg_flash_write(addr, (uint8_t)st->flash_cmd_val[0],
                                    st->dmg_write_cs_pulse);    /* PA=0x70 */
            if (!dmg_status_wait(st, addr, data[i])) {
                ok = 0u;
                break;
            }
            fw_cart_dmg_flash_write(addr, (uint8_t)st->flash_cmd_val[1],
                                    st->dmg_write_cs_pulse);    /* PA=0x10 */
            fw_cart_dmg_flash_write(addr, data[i],
                                    st->dmg_write_cs_pulse);    /* PA=PD   */
            if (!dmg_status_wait(st, addr, data[i])) {
                ok = 0u;
                break;
            }
        }
        bl_usb_poll();
    }
#if FW_DMG_UNLOCK_BYPASS && FW_DMG_WRITE_BURST
    /* Covers the break above as well: the chip must not be left in bypass. */
    if (bypass) {
        fw_cart_dmg_amd_bypass_exit(st->address);
    }
#endif
#if FW_DMG_WRITE_BURST
    /* The burst leaves D0..D7 driven; this covers the exits that do not end in
     * a poll. A data bus left driven is hazard H1. */
    fw_cart_dmg_write_burst_release();
#endif
    st->address += n;
    return ok;
}

/* FLASH_METHOD 2 on DMG, LK.c:1885-1962. buffer_size is in bytes on DMG and
 * halfwords on AGB (LK.c:1889 vs :1894). */
/* One buffer load at st->address + off, then its status wait. Shared by the
 * streaming pump and the tail, or the two would disagree on where a load
 * starts. */
static uint32_t dmg_program_chunk(fw_state_t *st, const uint8_t *data,
                                  uint32_t off, uint32_t have)
{
    uint32_t amd = (st->flash_command_set == 1u);
    uint32_t sa = st->address + off;
    uint32_t x;
    uint32_t used_burst = 0u;
#if FW_DMG_BUF_BURST && FW_DMG_WRITE_BURST
    const uint32_t buf_burst = (!st->flash_commands_bank_1
                                && !st->dmg_write_cs_pulse
                                && !st->flash_pulse_reset
                                && g_dmg_we_is_wr()) ? 1u : 0u;
#endif
#if FW_DMG_PROFILE
    uint32_t prof_t0 = prof_now();
#endif

#if FW_DMG_BUF_BURST && FW_DMG_WRITE_BURST
    if (amd && buf_burst) {
        fw_cart_dmg_amd_program_buffer(st->flash_cmd_addr, st->flash_cmd_val,
                                       sa, have, &data[off]);
        used_burst = 1u;
    }
#endif
    if (used_burst) {
        /* the whole load is done */
    } else if (amd) {
        fw_cart_dmg_flash_write(st->flash_cmd_addr[0],
                                (uint8_t)st->flash_cmd_val[0],
                                st->dmg_write_cs_pulse);    /* AAA=AA */
        fw_cart_dmg_flash_write(st->flash_cmd_addr[1],
                                (uint8_t)st->flash_cmd_val[1],
                                st->dmg_write_cs_pulse);    /* 555=55 */
        fw_cart_dmg_flash_write(sa, (uint8_t)st->flash_cmd_val[2],
                                st->dmg_write_cs_pulse);    /* SA=25  */
        fw_cart_dmg_flash_write(sa, (uint8_t)(have - 1u),
                                st->dmg_write_cs_pulse);    /* SA=BS  */
    } else {
        fw_cart_dmg_flash_write(sa, (uint8_t)st->flash_cmd_val[0],
                                st->dmg_write_cs_pulse);    /* SA=E8  */
        if (st->flash_sharp_verify_sr) {
            /* Sharp answers status before it means anything; wait a fixed
             * 10 us instead, 320 nops at 32 MHz. LK.c:1967-1972. */
            fw_cart_delay_nops(320u);
        } else if (!dmg_status_wait(st, sa, 0u)) {
            return 0u;
        }
        fw_cart_dmg_flash_write(sa, (uint8_t)(have - 1u),
                                st->dmg_write_cs_pulse);    /* SA=BS  */
    }

    if (!used_burst) {
        for (x = 0; x < have; x++) {
            fw_cart_dmg_flash_write(sa + x, data[off + x],
                                    st->dmg_write_cs_pulse);  /* PA=PD  */
        }
    }

    if (amd) {
        if (!used_burst) {
            fw_cart_dmg_flash_write(sa, (uint8_t)st->flash_cmd_val[5],
                                    st->dmg_write_cs_pulse);  /* SA=29  */
        }
#if FW_DMG_PROFILE
        {
            uint32_t d = prof_delta(prof_t0);
            if (g_prof_in_pump) { g_prof_cyc_pump += d; g_prof_pump_loads++; }
            else                { g_prof_cyc_load += d; g_prof_loads++; }
        }
        prof_t0 = prof_now();
#endif
        /* The last byte: an AMD buffer retires as a unit. LK.c:1957. */
        if (!dmg_status_wait(st, sa + have - 1u, data[off + have - 1u])) {
            return 0u;
        }
#if FW_DMG_PROFILE
        g_prof_cyc_poll += prof_delta(prof_t0);
#endif
    } else {
        fw_cart_dmg_flash_write(sa, (uint8_t)st->flash_cmd_val[3],
                                st->dmg_write_cs_pulse);    /* SA=D0  */
        if (!dmg_status_wait(st, sa, 0u)) {
            return 0u;
        }
        fw_cart_dmg_flash_write(sa, (uint8_t)st->flash_cmd_val[4],
                                st->dmg_write_cs_pulse);    /* SA=FF  */
    }
    return 1u;
}

/* Program what has arrived while the rest arrives, the DMG counterpart of
 * agb_stream_pump(). Buffered method 2 only. */
static void dmg_stream_pump(fw_state_t *st)
{
#if FW_DMG_WRITE_STREAM
    uint32_t filled = fw_proto_payload_filled();
    uint32_t chunk;

    if (filled == 0u) {
        return;
    }
    if (st->mode == FW_MODE_AGB || st->flash_method != 2u
        || st->buffer_size < 1u) {
        return;
    }
    if (st->program_stream_err) {
        return;
    }

    chunk = st->buffer_size;

#if FW_DMG_PROFILE
    g_prof_in_pump = 1u;
#endif
    while (filled >= (uint32_t)st->program_done + chunk) {
        if (st->program_done == 0u) {
            st->program_streamed = 0u;
        }
        bl_usb_poll();
        if (!dmg_program_chunk(st, fw_proto_payload(),
                               st->program_done, chunk)) {
            st->program_stream_err = 1u;
            break;
        }
        st->program_done = (uint16_t)(st->program_done + chunk);
        st->program_streamed++;
    }
#if FW_DMG_PROFILE
    g_prof_in_pump = 0u;
#endif
#else
    (void)st;
#endif
}

static uint32_t dmg_program_buffered(fw_state_t *st, const uint8_t *data,
                                     uint32_t n, uint32_t stream_done,
                                     uint32_t stream_err)
{
    uint32_t bs = st->buffer_size;
    uint32_t ok = 1u;
    uint32_t done = stream_done;

    /* The pump stopped on a failed load; resuming re-enters an aborted part. */
    if (stream_err) {
        st->address += n;
        return 0u;
    }

    while (done < n) {
        uint32_t have = n - done;

        if (have > bs) {
            have = bs;
        }
        if (!dmg_program_chunk(st, data, done, have)) {
            ok = 0u;
            break;
        }
        done += have;
        bl_usb_poll();
    }
    st->address += n;
    return ok;
}

/* Returns 0 when the chip timed out; the caller turns that into ACK_ERROR, and
 * the host answers by reading STATUS_REGISTER back. */
static uint32_t do_flash_program(fw_state_t *st)
{
#if FW_DMG_PROFILE
    uint32_t prof_cmd_t0 = prof_now();
    uint32_t prof_ret;
#define PROF_CMD_RET(v) do { prof_ret = (v); \
        g_prof_cyc_cmd += prof_delta(prof_cmd_t0); g_prof_cmds++; \
        return prof_ret; } while (0)
#else
#define PROF_CMD_RET(v) return (v)
#endif
    const uint8_t *data = fw_proto_payload();
    uint32_t n = st->program_len;
    uint32_t i;
    /* Cleared up front: a counter surviving into the next block makes it skip
     * that block's opening buffers. */
    uint32_t stream_done = st->program_done;
    uint32_t stream_err = st->program_stream_err;

    st->program_requested = 0u;
    st->program_done = 0u;
    st->program_stream_err = 0u;

    if (st->mode == FW_MODE_AGB) {
        /* SET_FLASH_CMD's six pairs are a template: the host sends SA/BS/PA/PD
         * as zero (LK_Device.py:3437-3441) and the device substitutes. Stock
         * sub_7704: [0][1] literal unlocks, [2] SA/0x25 buffer-load, [3]
         * SA/words-1, [4] PA/PD per word, [5] SA/0x29 confirm. SA is the chunk
         * base, inside the target sector by construction. */
        if (st->flash_method == 2u && st->buffer_size >= 2u) {
            uint32_t words = st->buffer_size / 2u;
            /* Resume where agb_stream_pump() left off; 0 when compiled out. */
            uint32_t done = stream_done;
            uint32_t ok = stream_err ? 0u : 1u;

            /* The pump timed out; resuming re-enters an aborted part. */
            if (stream_err) {
                st->address += n / 2u;
                return 0u;
            }

            /* Command set from SET_FLASH_CMD's first byte: 0x01 AMD, 0x02
             * Intel or Sharp (LK_Device.py:4223-4234), which the host does not
             * gate method 2 on. The branch is in agb_program_chunk(). */

            while (done + 1u < n) {
                uint32_t have = (n - done) / 2u;

                if (have > words) {
                    have = words;
                }
                if (!agb_program_chunk(st, data, done, have)) {
                    /* Stop: the part is in write-buffer-abort and each
                     * further buffer costs 500 ms against a 1 s host
                     * timeout. */
                    ok = 0u;
                    break;
                }
                done += have * 2u;
                bl_usb_poll();
            }
            /* No settle needed here: the loop's poll already waits on the
             * chunk's last word. */

            /* No per-block reset; it corrupts the tail. Stock resets after the
             * whole transfer (0x9E4A) and the host calls flashcart.Reset(). */
            st->address += n / 2u;
            return ok;
        } else {
        uint32_t ok1 = 1u;
        uint32_t amd1 = (st->flash_command_set == 1u);
        for (i = 0; i + 1u < n; i += 2u) {
            uint32_t k;
            uint32_t pa = st->address + (i / 2u);
            uint16_t w = (uint16_t)(data[i] | ((uint16_t)data[i + 1u] << 8));

            if (!amd1) {
                /* Intel and Sharp: replaying the six-slot table here would
                 * land 0x70 and 0x40 at ROM 0x08000000, the host having sent
                 * PA and PD as literal zero. LK.c:1864-1874. */
                fw_cart_agb_write(pa, st->flash_cmd_val[0]);        /* 0x70 */
                if (!agb_status_wait(st, pa, 0u)) {
                    ok1 = 0u;
                    break;
                }
                fw_cart_agb_write(pa, st->flash_cmd_val[1]);        /* 0x40 */
                fw_cart_agb_write(pa, w);                           /* PA=PD */
                if (!agb_status_wait(st, pa, w)) {
                    ok1 = 0u;
                    break;
                }
                bl_usb_poll();
                continue;
            }

            for (k = 0; k < 6u; k++) {
                if (st->flash_cmd_addr[k] == 0u && st->flash_cmd_val[k] == 0u) {
                    continue;           /* unused slot; the host zero-pads */
                }
                fw_cart_agb_write(st->flash_cmd_addr[k], st->flash_cmd_val[k]);
            }
            fw_cart_agb_write(pa, w);

            /* Wait for the word to retire, or the next unlock sequence lands in
             * a chip still programming the previous one. */
            if (!agb_status_wait(st, pa, w)) {
                ok1 = 0u;
                break;
            }

            /* Do not add a read-back poll: fw_cart_agb_peek() re-latches the
             * address, which a chip mid-program takes as a new bus cycle. */
            bl_usb_poll();
        }
        st->address += n / 2u;
        return ok1;
        }
    } else {
        /* method 2 with buffer_size 0 would loop on a zero-length chunk. */
        (void)i;
        if (st->flash_method == 2u && st->buffer_size >= 1u) {
            PROF_CMD_RET(dmg_program_buffered(st, data, n,
                                              stream_done, stream_err));
        }
        PROF_CMD_RET(dmg_program_unbuffered(st, data, n));
    }
}

/* Close the 3D Memory window and step a page: buffer_size / 2 once, not
 * transfer_size per read (stock 0x6466-0x6472). */
static void do_3d_page_end(fw_state_t *st)
{
    if (st->page_bytes_done == 0u) {
        return;
    }
    st->page_bytes_done = 0u;
    /* Do not clear page_terminator_due here; proto.c:530 does. ACKing the bare
     * 0x00 that closes a page shifts every later page of the dump. */
    fw_cart_agb_3d_close();
    st->address += st->buffer_size / 2u;
}

/* Two granularities: the POLL step is how often the SIE is serviced, the LATCH
 * interval how often /CS is re-asserted. Do not conflate them; 128 is stock's
 * interval. Cross-command prefetch was tried and is slower. */
static void do_cart_read(fw_state_t *st)
{
    cart_wait_ready();
    st->pump_stalls = 0u;
#if FW_TX_DIRECT
    /* Hand the endpoint g_reply itself; see tx_pump() in src/usb.c. */
    __extension__ _Static_assert(FW_MAX_TRANSFER <= 0xFFFFu,
                                 "do_cart_read: a direct transfer is indexed "
                                 "by a uint16_t");
    bl_usb_tx_direct_begin(g_reply, (uint16_t)st->transfer_size);
#endif
    uint32_t len = st->transfer_size;
    uint32_t step = agb_step_bytes(st);
    uint32_t latch = agb_latch_bytes(st);


    uint32_t off = 0u;
    uint32_t sent = 0u;


#if FW_AGB_LEAF
#if FW_STREAM_MIDGROUP_POLL
#error "FW_AGB_LEAF and FW_STREAM_MIDGROUP_POLL both restructure this loop. \
The leaf cannot have a mid-group poll: it would have to happen inside a leaf \
that never returns. Pick one."
#endif
    /* One leaf call per `step' bytes, whole latch groups only, or every later
     * group starts at the wrong address. No `%': Cortex-M0 has no divide. */
    uint32_t grp_hw = latch >> 1;
    uint32_t per_call;

    if (grp_hw == 0u) {
        grp_hw = 1u;                    /* an odd cart_latch cannot be 0 hw */
    }
    latch = grp_hw << 1;
    /* Largest multiple of latch that fits in step. By repeated addition this
     * ran 255 iterations per read command at the host's default Single latch of
     * 2. cart_latch is any multiple of 32, so 96 is reachable and the mask form
     * is only valid when latch is a power of two. */
    if ((latch & (latch - 1u)) == 0u) {
        per_call = step & ~(latch - 1u);
        if (per_call < latch) {
            per_call = latch;
        }
    } else {
        per_call = latch;
        while ((per_call + latch) <= step) {
            per_call += latch;
        }
    }

    while (off < len) {
        uint32_t chunk = len - off;
        uint32_t hw;

        if (chunk > per_call) {
            chunk = per_call;
        }
        hw = chunk >> 1;
        if (hw != 0u) {
            /* g_reply is 4-aligned and `off' advances by a multiple of 2, so
             * the strh destination is aligned. */
            fw_cart_agb_read_leaf(st->address + (off / 2u),
                                  (uint16_t *)(void *)&g_reply[off],
                                  hw, grp_hw);
        }
        if ((chunk & 1u) != 0u) {
            /* The host is committed to transfer_size bytes; pad to width. */
            g_reply[off + chunk - 1u] = 0xFFu;
        }
        off += chunk;
#else
    while (off < len) {
        uint32_t grp = len - off;
        uint32_t g = 0u;

        if (grp > latch) {
            grp = latch;
        }
        fw_cart_agb_open(st->address + (off / 2u));

        /* Nothing but bus cycles between latch and deselect: a pump here holds
         * /CS across a transfer and corrupts Single and MemCpy, not Stream. */
#if FW_STREAM_MIDGROUP_POLL
        uint32_t mid = (grp >= (2u * (uint32_t)bl_usb_ep2_pkt_in))
                     ? (uint32_t)bl_usb_ep2_pkt_in : 0u;
#endif
        while (g < grp) {
            uint32_t want = grp - g;
            uint8_t *p = &g_reply[off + g];
            uint32_t got;

            if (want > step) {
                want = step;
            }
#if FW_STREAM_MIDGROUP_POLL
            /* Chunk the burst or the poll below never runs: step == latch. */
            if (mid && (want > mid)) {
                want = mid;
            }
#endif
            got = fw_cart_agb_burst(p, want);
            /* The host is committed to transfer_size bytes; pad to width. */
            while (got < want) {
                p[got++] = 0xFFu;       /* open bus */
            }
            g += want;
#if FW_STREAM_MIDGROUP_POLL
            /* Publication is a byte-count store; it cannot touch the bus. */
            if (mid && (g < grp) && ((off + g - sent) >= mid)) {
                bl_usb_tx_direct_publish((uint16_t)(off + g));
                bl_usb_poll();
                if ((off + g - sent) >= mid) {
                    sent += mid;
                }
            }
#endif
        }

        fw_cart_agb_close();
        off += grp;
#endif /* FW_AGB_LEAF */

        /* Pump what has accumulated, not exactly one step. Filling the whole
         * transfer first is slower, and so is raising `step' to the latch
         * size. */
#if FW_TX_DIRECT
        /* One bl_usb_poll() per `step' bytes. With FW_USB_IRQ armed it leaves
         * the SIE alone and is only the thread-mode heartbeat. */
        if ((off - sent) >= step) {
            bl_usb_tx_direct_publish((uint16_t)off);
            while ((off - sent) >= step) {
                bl_usb_poll();
                sent += step;
            }
        }
    }

    bl_usb_tx_direct_publish((uint16_t)len);

    /* FW_TX_STALL_MS with the SIE taking nothing means the host is gone. */
    {
        uint32_t deadline = 0u;
        uint8_t  armed = 0u;
        uint16_t seen = bl_usb_tx_direct_sent();
        while (bl_usb_tx_direct_sent() < (uint16_t)len) {
            uint16_t now_sent;

            bl_usb_poll();
            now_sent = bl_usb_tx_direct_sent();
            if (now_sent != seen) {
                seen  = now_sent;
                armed = 0u;
            } else if (armed == 0u) {
                deadline = bl_time_ms() + FW_TX_STALL_MS;
                armed = 1u;
            } else if ((int32_t)(bl_time_ms() - deadline) >= 0) {
                if (st->pump_stalls != 0xFFFFu) {
                    st->pump_stalls++;
                }
                bl_usb_tx_direct_end();
                return;
            }
        }
    }

    bl_usb_tx_direct_end();
#else
        if ((off - sent) >= step) {
            uint32_t ready = off - sent;
            if (!pump(&g_reply[sent], ready)) {
                return;
            }
            sent += ready;
        }
    }

    (void)pump(&g_reply[sent], len - sent);
#endif

    /* Track the cartridge's own advance; the host sets ADDRESS once. */
    st->address += (len / 2u);
}

#if FW_USB_IRQ
/* The interrupt transport's watchdog. Demotion is one way; bl_usb_irq_arm()
 * refuses once faulted, and bl_usb_tx_pending() gates both steps so an idle
 * link cannot demote itself in its first pause.
 * soft: mask IRQ6 and poll instead. USB events are latching flags, so the next
 *       bl_usb_poll() resumes with no re-enumeration.
 * hard: bl_usb_init(), which drops D+ and re-attaches; visible to the host. */

/* Soft-demote after this long owing the wire bytes with no handler progress.
 * Past the host's 1-1.5 s command timeout and past FW_TX_STALL_MS. */
#ifndef FW_USB_IRQ_WEDGE_MS
#define FW_USB_IRQ_WEDGE_MS   4000u
#endif

/* No serviced event at all, in either mode, before re-enumerating. The only
 * part of the watchdog visible to the host; =0 compiles the hard step out. */
#ifndef FW_USB_IRQ_DEAD_MS
#define FW_USB_IRQ_DEAD_MS   15000u
#endif

static uint32_t g_irq_mark;
static uint32_t g_irq_deadline;
#if FW_USB_IRQ_DEAD_MS
static uint32_t g_evt_mark;
static uint32_t g_evt_deadline;
#endif

static void fw_usb_irq_tick(void)
{
    uint32_t now = bl_time_ms();

#if FW_USB_IRQ_DEAD_MS
    /* The hard step, and it watches both modes. */
    if ((bl_usb_events() != g_evt_mark) || !bl_usb_tx_pending()) {
        g_evt_mark = bl_usb_events();
        g_evt_deadline = now + FW_USB_IRQ_DEAD_MS;
    } else if ((int32_t)(now - g_evt_deadline) >= 0) {
        g_evt_deadline = now + FW_USB_IRQ_DEAD_MS;
        bl_usb_irq_disarm(BL_USB_IRQ_FAULT_REINIT);
        bl_usb_init();
        return;
    }
#endif

    if (!bl_usb_irq_armed()) {
        if (bl_usb_configured()
                && (bl_usb_irq_fault() == BL_USB_IRQ_FAULT_NONE)) {
            (void)bl_usb_irq_arm();
            g_irq_mark = bl_usb_irq_entries();
            g_irq_deadline = now + FW_USB_IRQ_WEDGE_MS;
        }
        return;
    }

    if ((bl_usb_irq_entries() != g_irq_mark) || !bl_usb_tx_pending()) {
        g_irq_mark = bl_usb_irq_entries();
        g_irq_deadline = now + FW_USB_IRQ_WEDGE_MS;
    } else if ((int32_t)(now - g_irq_deadline) >= 0) {
        bl_usb_irq_disarm(BL_USB_IRQ_FAULT_WEDGE);
    }
}
#endif /* FW_USB_IRQ */

#ifndef FW_RX_LOAD_NOPS
#define FW_RX_LOAD_NOPS 0       /* diagnostic; see the receive loop below */
#endif

void fw_main(void)
{
#if FW_DMG_PROFILE
    fw_prof_stack_fill();
#endif
    /* One pass of the loop drains at most this much, so it also sets how often
     * the loop's fixed prologue (link supervision, deferred-work scan, parser
     * entry) is paid: at 64 a 2048-byte block pays it 32 times. */
    uint8_t rx[FW_RX_BUF_BYTES];
    int was_configured;

    bl_time_init();
    fw_proto_init(&g_state);
    g_state.pcb_ver = probe_pcb_version();

    fw_cart_init();

    /* Park the activity LED lit (PB12, active low): that is how a human tells
     * application from bootloader. LK parks it off; fw_lk_dispatch re-parks. */
    fw_lk_led_idle();

    fw_lk_init(&g_state);
    bl_usb_init();

    /* Cartridge chunk size. Polled, read the live bulk IN packet size so the
     * two cannot drift apart. */
#if FW_USB_IRQ
    /* With the interrupt armed the handler drains the ring while the loop is on
     * the bus, so a chunk above one packet costs nothing. 512 is the swept
     * peak; CMD_FW_SET_STEP overrides it. */
    g_state.cart_step = 512u;
#else
    g_state.cart_step = bl_usb_ep2_pkt_in;
#endif
#if FW_USB_CDC
    /* VAR16_FW_USB_DESC; the identity is fixed at compile time. */
    g_state.usb_desc_state = BL_USB_DESC_SET_CDC;
#endif
    was_configured = bl_usb_configured();

    for (;;) {
        uint32_t n;
        uint32_t i;

        /* Advances enumeration and the endpoints. The call stays because the
         * layer may demote back to polling. */
        bl_usb_poll();

#if FW_USB_IRQ
        fw_usb_irq_tick();

        /* VAR16_FW_USB_IRQ: low byte armed, high byte the sticky fault code.
         * proto.c cannot include usb.h, so main.c bridges it. */
        g_state.usb_irq_state = (uint16_t)((bl_usb_irq_armed() ? 1u : 0u)
                                | ((uint16_t)bl_usb_irq_fault() << 8));
#else
        g_state.usb_irq_state = 0u;
#endif

        /* A half-received command must not outlive the host or the link. */
        fw_proto_tick(&g_state, bl_time_ms());
        {
            int cfg = bl_usb_configured();
            if (cfg != was_configured) {
                was_configured = cfg;
                fw_proto_link_reset(&g_state);
            }
        }

        n = bl_usb_rx(rx, sizeof(rx));

        /* Do not leave the receive loop mid-payload; the tick and deferred-work
         * scan cost more than they do. FW_PARSER_TIMEOUT_MS bounds the wait. */
        if ((n == 0u) && (fw_proto_payload_remaining() != 0u)) {
            uint32_t deadline = bl_time_ms() + FW_PARSER_TIMEOUT_MS;
            while ((n == 0u) && (fw_proto_payload_remaining() != 0u)) {
                /* The endpoint is empty mid-block: flash-chip time. */
                agb_stream_pump(&g_state);
                dmg_stream_pump(&g_state);
                bl_usb_poll();
                n = bl_usb_rx(rx, sizeof(rx));
                if ((n == 0u) && ((int32_t)(bl_time_ms() - deadline) >= 0)) {
                    break;
                }
            }
        }

        /* Single buffered, EP2's one OUT window stays open across an unpolled
         * batch and the host overwrites it; poll every byte, 16 when dbuf. */
        {
            uint32_t poll_mask = bl_usb_ep2_dbuf ? 15u : 0u;

        i = 0u;
        while (i < n) {
            uint32_t out;
            uint32_t took = 0u;

            if ((i & poll_mask) == 0u) {
                bl_usb_poll();
            }

            /* The LK seam: opcodes in fw_lk_routes() go to lk_loop(), the
             * rest to the parser. The fw_proto_idle() guard must stay a
             * property of parser state, not of the byte: without it a payload
             * byte equal to an opcode put 5 V on a 3.3 V cart. */
            if (fw_proto_idle() && fw_lk_routes(&g_state, rx[i])) {
                uint32_t used = fw_lk_dispatch(&g_state, rx[i],
                                               &rx[i + 1u], n - i - 1u);
                i += 1u + used;
                /* lk_loop() emitted its own reply and sets no *_requested. */
                continue;
            }

            /* A run of payload bytes at once, otherwise one byte, so anything
             * that could be an opcode sees the full state machine. */
            out = fw_proto_feed_bulk(&g_state, &rx[i], n - i, g_reply, &took);
            if (took != 0u) {
                i += took;
                /* Diagnostic load: unchanged throughput means the receive
                 * ceiling is the host, not this CPU. */
#if FW_RX_LOAD_NOPS
                {
                    uint32_t z = took * (uint32_t)FW_RX_LOAD_NOPS;
                    while (z--) {
                        __asm__ volatile("nop");
                    }
                }
#endif
                agb_stream_pump(&g_state);
                dmg_stream_pump(&g_state);
            } else {
                out = fw_proto_feed(&g_state, rx[i], g_reply);
                i++;
            }
            if (g_state.audio_requested) {
                g_state.audio_requested = 0u;
                fw_cart_audio_drive(g_state.mode == FW_MODE_AGB
                                    ? g_state.agb_irq_enabled
                                    : g_state.dmg_audio_enabled);
            }
            if (g_state.we_pin_requested) {
                g_state.we_pin_requested = 0u;
                fw_cart_set_we_pin(g_state.flash_we_pin_var);
            }
            if (g_state.pullup_requested) {
                g_state.pullup_requested = 0u;
                fw_cart_pullups(g_state.pullups_enabled);
            }
            /* Raw (address, value) pairs from FlashGBX's cartridge base. */
            if (g_state.batch_requested) {
                cart_wait_ready();
                uint32_t k;
                g_state.batch_requested = 0u;
                /* The flashcart byte selects the bus (LK_Device.py:744): set is
                 * ROM space, clear the SRAM space behind /CS2, where a FLASH 1M
                 * bank switch must land or a 128 KiB backup is bank 0 twice. */
                if (g_state.mode != FW_MODE_DMG && !g_state.batch_flashcart) {
                    fw_cart_agb_sram_open();
                }
                for (k = 0; k < g_state.batch_count; k++) {
                    if (g_state.mode == FW_MODE_DMG) {
                        /* DMG differs on the strobe, not the bus; only the
                         * flashcart arm reads FLASH_WE_PIN. */
                        if (g_state.batch_flashcart) {
                            fw_cart_dmg_flash_write(g_state.batch_addr[k],
                                          (uint8_t)g_state.batch_val[k],
                                          g_state.dmg_write_cs_pulse);
                        } else {
                            fw_cart_dmg_write(g_state.batch_addr[k],
                                          (uint8_t)g_state.batch_val[k],
                                          g_state.dmg_write_cs_pulse);
                        }
                    } else if (g_state.batch_flashcart) {
                        fw_cart_agb_write(g_state.batch_addr[k],
                                          g_state.batch_val[k]);
                    } else {
                        fw_cart_agb_sram_write(g_state.batch_addr[k],
                                               (uint8_t)g_state.batch_val[k]);
                    }
                }
                if (g_state.mode != FW_MODE_DMG && !g_state.batch_flashcart) {
                    fw_cart_agb_sram_close();
                }
            }
            if (g_state.flash_write_requested) {
                g_state.flash_write_requested = 0u;
                cart_wait_ready();
                if (g_state.flash_write_is_agb) {
                    fw_cart_agb_write(g_state.flash_write_addr,
                                      g_state.flash_write_val);
                } else {
                    /* 0xD1 is flashcart-targeted; stock sub_8790. */
                    fw_cart_dmg_flash_write(g_state.flash_write_addr,
                                      (uint8_t)g_state.flash_write_val,
                                      g_state.dmg_write_cs_pulse);
                }
            }
            if (g_state.program_requested) {
                if (!do_flash_program(&g_state) && out == 1u) {
                    /* Deferred work runs before the reply is pumped. */
                    g_reply[0] = ACK_ERROR;
                }
            }
            if (g_state.save_write_requested) {
                do_save_write(&g_state);
            }
            if (g_state.crc_requested) {
                do_crc32(&g_state, g_reply);
                out = 4u;       /* the CRC, big-endian; see do_crc32() */
            }
            if (g_state.write_requested) {
                g_state.write_requested = 0u;
                cart_wait_ready();
                fw_cart_dmg_write(g_state.write_addr, g_state.write_value,
                                  g_state.dmg_write_cs_pulse);
            }
            if (g_state.mbc_reset_requested) {
                g_state.mbc_reset_requested = 0u;
                fw_cart_dmg_mbc_reset();
            }
            if (g_state.debug_requested) {
                g_state.debug_requested = 0u;
                fw_cart_clk_pulses(20u);
            }
            if (g_state.clk_requested) {
                g_state.clk_requested = 0u;
                fw_cart_clk_pulses(g_state.clk_pulses);
            }
            if (g_state.voltage_requested) {
                g_state.voltage_requested = 0u;
                g_state.voltage_is_five = (uint8_t)fw_cart_voltage(
                    g_state.voltage_five,
                    g_state.mode == FW_MODE_DMG);
            }
            if (g_state.tristate_requested) {
                g_state.tristate_requested = 0u;
                fw_cart_tristate();
            }
            if (g_state.power_requested) {
                g_state.power_requested = 0u;
                fw_cart_power(g_state.power_target);
                g_state.cart_powered = g_state.power_target;
                if (g_state.power_target) {
                    /* This much before the ACK; the rest on g_cart_ready_at. */
                    uint32_t until = bl_time_ms() + FW_CART_POWERON_ACK_MS;
                    while ((int32_t)(bl_time_ms() - until) < 0) {
                        bl_usb_poll();
                    }
                    g_cart_ready_at = bl_time_ms()
                        + (FW_CART_POWERON_MS - FW_CART_POWERON_ACK_MS);
                }
                if (g_state.power_target) {
                    /* Re-apply the standing voltage: cart.c drops PB_VSEL on
                     * power-off, a latched 5 V rail damaging the next cartridge
                     * inserted, and CartPowerOn sends no SET_VOLTAGE. */
                    g_state.voltage_is_five = (uint8_t)fw_cart_voltage(
                        g_state.voltage_five,
                        g_state.mode == FW_MODE_DMG);
                } else {
                    /* Keep voltage_five; the request outlives a power cycle. */
                    g_state.voltage_is_five = 0u;
                }
            }
            if (g_state.bench_requested) {
                /* The transport with the bus taken out. Keep it on the direct
                 * region, or it measures slower than a real ROM read. */
                uint32_t remaining = g_state.transfer_size;
                g_state.bench_requested = 0u;
#if FW_TX_DIRECT
                {
                    uint32_t off = 0u;

                    bl_usb_tx_direct_begin(g_reply, (uint16_t)remaining);
                    while (off < remaining) {
                        uint32_t step = step_bytes(&g_state);
                        uint32_t want = (remaining - off > step)
                                        ? step : (remaining - off);
                        off += want;
                        bl_usb_tx_direct_publish((uint16_t)off);
                        {
                            uint32_t k = want;
                            while (k != 0u) {
                                bl_usb_poll();
                                k = (k > 64u) ? (k - 64u) : 0u;
                            }
                        }
                    }
                    {
                        uint32_t deadline = 0u;
                        uint8_t  armed = 0u;
                        uint16_t seen = bl_usb_tx_direct_sent();

                        while (bl_usb_tx_direct_sent() < (uint16_t)remaining) {
                            uint16_t now_sent;

                            bl_usb_poll();
                            now_sent = bl_usb_tx_direct_sent();
                            if (now_sent != seen) {
                                seen = now_sent;
                                armed = 0u;
                            } else if (armed == 0u) {
                                deadline = bl_time_ms() + FW_TX_STALL_MS;
                                armed = 1u;
                            } else if ((int32_t)(bl_time_ms() - deadline) >= 0) {
                                break;
                            }
                        }
                    }
                    bl_usb_tx_direct_end();
                }
#else
                while (remaining) {
                    uint32_t step = step_bytes(&g_state);
                    uint32_t want = (remaining > step) ? step : remaining;
                    if (!pump(g_reply, want)) {
                        remaining = 0u;
                        break;
                    }
                    remaining -= want;
                }
#endif
                out = 0u;
            }
            if (g_state.bootup_requested) {
                g_state.bootup_requested = 0u;
                cart_wait_ready();
                fw_cart_agb_bootup();
            }
            if (g_state.rtc_read_requested) {
                /* The reply length is already committed at 8. */
                g_state.rtc_read_requested = 0u;
                cart_wait_ready();
                fw_cart_agb_rtc_read(g_reply);
            }
            if (g_state.page_end_requested) {
                g_state.page_end_requested = 0u;
                do_3d_page_end(&g_state);
            }
            if (g_state.read_requested) {
                /* Emit transfer_size bytes whatever the bus returned; a short
                 * reply desynchronises every later command. */
                g_state.read_requested = 0u;
                if (g_state.read_is_3d) {
                    /* One page over several commands: opened on its first 0xC8,
                     * closed on the terminator or buffer_size/transfer_size. */
                    g_state.read_is_3d = 0u;
                    if (g_state.page_bytes_done == 0u) {
                        cart_wait_ready();
                        fw_cart_agb_3d_open(g_state.address,
                                            g_state.buffer_size);
                    }
#if FW_M3D_TX_DIRECT
                    /* The only read path that still strobes a whole page off
                     * the cartridge before offering a byte to the endpoint.
                     * fw_cart_agb_3d_read has no MMIO outside its strobe loop,
                     * so splitting the call emits the same /RD cycles. */
                    m3d_read_overlapped(&g_state);
#else
                    (void)fw_cart_agb_3d_read(g_reply, g_state.transfer_size);
                    (void)pump(g_reply, g_state.transfer_size);
#endif
                    g_state.page_bytes_done += (uint16_t)g_state.transfer_size;
                    if (g_state.page_bytes_done >= g_state.buffer_size) {
                        do_3d_page_end(&g_state);
                    }
                } else if (g_state.read_is_eeprom) {
                    g_state.read_is_eeprom = 0u;
                    do_eeprom_read(&g_state);
                } else if (g_state.read_is_save) {
                    g_state.read_is_save = 0u;
                    do_save_read(&g_state);
                } else if (g_state.mode == FW_MODE_DMG) {
                    do_dmg_read(&g_state);
                } else {
#if defined(FW_AGB_READ_BATCH) && FW_AGB_READ_BATCH
                    /* A read opcode leaves the parser idle, so the next byte
                     * of this receive batch is another opcode. Take a run of
                     * them here and stream the lot: the host round trip behind
                     * each opcode is what a dump spends between chunks, and
                     * only the first one is paid. The nominated region is
                     * still bounded by g_reply, so a merged length above that
                     * goes out as successive regions with no opcode between. */
                    uint16_t base = g_state.transfer_size;
                    uint32_t reps = 1u;
                    uint32_t total;

                    while ((reps < (uint32_t)(FW_AGB_READ_BATCH)) && (i < n)
                           && (rx[i] == CMD_AGB_CART_READ)) {
                        reps++;
                        i++;
                    }
                    total = (uint32_t)base * reps;
                    do {
                        uint32_t lap = total;
                        uint32_t was = g_state.address;

                        if (lap > (uint32_t)sizeof(g_reply)) {
                            lap = (uint32_t)sizeof(g_reply);
                        }
                        g_state.transfer_size = (uint16_t)lap;
                        do_cart_read(&g_state);
                        total -= lap;
                        /* No advance is do_cart_read()'s stall exit: the host
                         * has stopped reading and the rest is owed to nobody. */
                        if ((lap != 0u) && (g_state.address == was)) {
                            break;
                        }
                    } while (total != 0u);
                    g_state.transfer_size = base;
#else
                    do_cart_read(&g_state);
#endif
                }
                out = 0u;               /* do_cart_read() sent it all */
            }
            if (out) {
                uint32_t sent = 0;
                (void)pump(&g_reply[sent], out - sent);
            }

            /* Last, after the reply is queued, or BOOTLOADER_RESET's ACK is
             * lost and the host fails a request that succeeded. */
            if (g_state.reset_requested) {
                enter_update_mode();     /* does not return */
            }
        }
        }
    }
}
