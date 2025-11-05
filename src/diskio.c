#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "ff.h"
#include "diskio.h"

#define SPI_PORT spi0
#define PIN_MISO 16
#define PIN_CS   17
#define PIN_SCK  18
#define PIN_MOSI 19

#define SD_BLOCK_SIZE 512

static volatile DSTATUS Stat = STA_NOINIT;
static int is_sdhc = 0; // flag para indicar cartão SDHC/SDXC (endereçamento em LBA)

static void sd_select(void) {
    gpio_put(PIN_CS, 0);
}

static void sd_deselect(void) {
    gpio_put(PIN_CS, 1);
    spi_write_read_blocking(SPI_PORT, (uint8_t[]){0xFF}, NULL, 1);
}

static uint8_t spi_transfer(uint8_t data) {
    uint8_t rx;
    spi_write_read_blocking(SPI_PORT, &data, &rx, 1);
    return rx;
}

static uint8_t sd_wait_ready(void) {
    uint8_t res;
    for (int i = 0; i < 500; i++) {
        res = spi_transfer(0xFF);
        if (res == 0xFF) return 1;
        sleep_us(1000);
    }
    return 0;
}

static void sd_send_clock_train(void) {
    sd_deselect();
    for (int i = 0; i < 10; i++) {
        spi_transfer(0xFF);
    }
}

static uint8_t sd_command(uint8_t cmd, uint32_t arg) {
    uint8_t crc = 0x01;
    uint8_t res;

    if (cmd == 0) crc = 0x95;
    if (cmd == 8) crc = 0x87;

    sd_deselect();
    sd_select();

    if (!sd_wait_ready()) {
        sd_deselect();
        return 0xFF;
    }

    spi_transfer(0x40 | cmd);
    spi_transfer((uint8_t)(arg >> 24));
    spi_transfer((uint8_t)(arg >> 16));
    spi_transfer((uint8_t)(arg >> 8));
    spi_transfer((uint8_t)arg);
    spi_transfer(crc);

    for (int i = 0; i < 10; i++) {
        res = spi_transfer(0xFF);
        if (!(res & 0x80)) return res;
    }
    return 0xFF;
}

static uint8_t sd_acmd(uint8_t cmd, uint32_t arg) {
    uint8_t res = sd_command(55, 0);
    if (res > 1) return res;
    return sd_command(cmd, arg);
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;

    spi_init(SPI_PORT, 1000 * 1000);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);

    sd_send_clock_train();

    uint8_t res;

    res = sd_command(0, 0); // CMD0
    printf("CMD0 response: 0x%02X\n", res);
    if (res != 1) return STA_NOINIT;

    res = sd_command(8, 0x1AA); // CMD8
    printf("CMD8 response: 0x%02X\n", res);

    if (res == 1) {
        // SD v2+
        for (int i = 0; i < 4; i++) spi_transfer(0xFF); // lê 4 bytes da resposta do CMD8

        int timeout = 1000;
        do {
            res = sd_acmd(41, 0x40000000);
            printf("ACMD41 response: 0x%02X\n", res);
            sleep_ms(1);
            if (--timeout == 0) break;
        } while (res != 0);

        if (res != 0) return STA_NOINIT;

        res = sd_command(58, 0);
        printf("CMD58 response: 0x%02X\n", res);
        if (res != 0) return STA_NOINIT;

        uint8_t ocr[4];
        for (int i = 0; i < 4; i++) ocr[i] = spi_transfer(0xFF);
        // Verifica se é SDHC (bit 6 do primeiro byte da OCR)
        if (ocr[0] & 0x40) {
            is_sdhc = 1;
            printf("Cartão SDHC/SDXC detectado\n");
        } else {
            is_sdhc = 0;
            printf("Cartão SDSC detectado\n");
        }
    } else {
        // SD v1 ou MMC fallback
        int timeout = 1000;
        do {
            res = sd_acmd(41, 0);
            printf("ACMD41 fallback response: 0x%02X\n", res);
            sleep_ms(1);
            if (--timeout == 0) break;
        } while (res != 0);

        if (res != 0) return STA_NOINIT;

        is_sdhc = 0;
    }

    Stat &= ~STA_NOINIT;
    return Stat;
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv == 0) return Stat;
    return STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !count) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;

    while (count--) {
        uint32_t address = is_sdhc ? sector : sector * SD_BLOCK_SIZE;

        if (sd_command(17, address) != 0) return RES_ERROR;

        // Aguarda token 0xFE com timeout
        int timeout = 10000;
        uint8_t token;
        do {
            token = spi_transfer(0xFF);
            if (token == 0xFE) break;
            sleep_us(10);
        } while (--timeout);

        if (token != 0xFE) return RES_ERROR;

        for (int i = 0; i < SD_BLOCK_SIZE; i++) {
            buff[i] = spi_transfer(0xFF);
        }
        buff += SD_BLOCK_SIZE;

        spi_transfer(0xFF); // CRC
        spi_transfer(0xFF);
        sector++;
    }

    return RES_OK;
}

#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !count) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;

    while (count--) {
        uint32_t address = is_sdhc ? sector : sector * SD_BLOCK_SIZE;

        if (sd_command(24, address) != 0) return RES_ERROR;

        spi_transfer(0xFF);
        spi_transfer(0xFE);

        for (int i = 0; i < SD_BLOCK_SIZE; i++) {
            spi_transfer(buff[i]);
        }
        buff += SD_BLOCK_SIZE;

        spi_transfer(0xFF);
        spi_transfer(0xFF);

        uint8_t resp = spi_transfer(0xFF);
        if ((resp & 0x1F) != 0x05) return RES_ERROR;

        while (spi_transfer(0xFF) == 0) ; // espera fim

        sector++;
    }

    return RES_OK;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0) return RES_PARERR;

    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = SD_BLOCK_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(DWORD *)buff = 32768; // exemplo para cartão 16MB
        return RES_OK;
    }

    return RES_PARERR;
}
