#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/cyw43_arch.h"
#include "lwip/altcp_tls.h"
#include "example_http_client_util.h"

// SD card libs
#include "ff.h"

#define UART_ID uart0
#define BAUD_RATE 9600
#define UART_TX_PIN 0
#define UART_RX_PIN 1

// SPI Pins (conforme você informou)
#define PIN_MISO 16
#define PIN_CS 17
#define PIN_SCK 18
#define PIN_MOSI 19
#define SPI_PORT spi0

#define LED_ON_PIN 11
#define LED_OFF_PIN 13

#define BUTTON_A 5 // GPIO do botão

#define HOST "server-findway.onrender.com"
#define URL_REQUEST "/mensagem?msg="
#define BUFFER_SIZE 512

void send_data(const char *data);

char gps_buffer[BUFFER_SIZE];
char http_response[BUFFER_SIZE];

volatile bool led_on = false;
volatile bool led_state_changed = false;
absolute_time_t last_button_time;
bool last_button_state = true;

// Variáveis do SD
FATFS fs;
FIL fil;
FRESULT fr;
UINT bw;

DWORD get_fattime(void)
{
    return ((DWORD)(2025 - 1980) << 25) | ((DWORD)7 << 21) | ((DWORD)9 << 16) | ((DWORD)16 << 11) | ((DWORD)45 << 5) | ((DWORD)0 >> 1);
}

// Inicializa SD (agora com inicialização da SPI)
void init_sd()
{
    // Inicializa SPI para o SD (config inicial segura, pode ajustar velocidade)
    spi_init(SPI_PORT, 4 * 1000 * 1000);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    // CS como GPIO de saída (ativa em low)
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1); // deixa CS em HIGH (inativo)

    // Pequeno atraso antes de montar
    sleep_ms(100);

    fr = f_mount(&fs, "", 1);
    if (fr != FR_OK)
    {
        printf("Falha ao montar SD. Código: %d\n", fr);
    }
    else
    {
        printf("Cartão SD montado com sucesso.\n");
    }
}

// Salva string no SD
void salvar_no_sd(const char *dados)
{
    fr = f_open(&fil, "gps_log.txt", FA_OPEN_APPEND | FA_WRITE);
    if (fr == FR_OK)
    {
        f_write(&fil, dados, strlen(dados), &bw);
        f_close(&fil);
    }
    else
    {
        printf("Erro ao abrir arquivo no SD. Código: %d\n", fr);
    }
}

// Codifica string para URL
void urlencode(const char *input, char *output, size_t output_size)
{
    char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; input[i] && j + 3 < output_size; i++)
    {
        if ((input[i] >= 'A' && input[i] <= 'Z') ||
            (input[i] >= 'a' && input[i] <= 'z') ||
            (input[i] >= '0' && input[i] <= '9') ||
            input[i] == '-' || input[i] == '_' || input[i] == '.' || input[i] == '~')
        {
            output[j++] = input[i];
        }
        else
        {
            output[j++] = '%';
            output[j++] = hex[(input[i] >> 4) & 0xF];
            output[j++] = hex[input[i] & 0xF];
        }
    }
    output[j] = '\0';
}
err_t http_receive_cb(void *arg, struct altcp_pcb *pcb,
                      struct pbuf *p, err_t err)
{

    if (!p)
        return ERR_OK;

    memset(http_response, 0, BUFFER_SIZE);
    pbuf_copy_partial(p, http_response, p->tot_len, 0);

    printf("Resposta do servidor:\n%s\n", http_response);

    if (strstr(http_response, "\"state\": \"on\""))
    {
        led_on = true;
        led_state_changed = true;
    }
    else if (strstr(http_response, "\"state\": \"off\""))
    {
        led_on = false;
        led_state_changed = true;
    }

    pbuf_free(p);
    return ERR_OK;
}
void atualizar_leds()
{
    if (!led_state_changed)
        return;

    if (led_on)
    { // Led verde ligado
        gpio_put(LED_ON_PIN, 1);
        gpio_put(LED_OFF_PIN, 0);
        printf("LED 11 LIGADO 🟢 | LED 13 DESLIGADO ⚫\n");
    }
    else
    {
        gpio_put(LED_ON_PIN, 0);
        gpio_put(LED_OFF_PIN, 1);
        printf("LED 11 DESLIGADO ⚫ | LED 13 LIGADO 🔴\n");
    }

    led_state_changed = false;
}
void verificar_botao()
{
    bool estado_atual = gpio_get(BUTTON_A);

    if (last_button_state && !estado_atual)
    {
        if (absolute_time_diff_us(last_button_time, get_absolute_time()) > 250000)
        {

            led_on = !led_on;
            led_state_changed = true;

            printf("Botão pressionado → LED %s (enviando ao servidor)\n",
                   led_on ? "ON 🟢" : "OFF 🔴");

            // 📡 Envia estado do botão para o servidor
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "placa=LUV123, led=%s, origem=botao",
                     led_on ? "on" : "off");

            send_data(msg);

            last_button_time = get_absolute_time();
        }
    }

    last_button_state = estado_atual;
}

// Envia dados para o servidor
void send_data(const char *data)
{
    char encoded_data[BUFFER_SIZE];
    urlencode(data, encoded_data, BUFFER_SIZE);

    char full_url[BUFFER_SIZE];
    snprintf(full_url, BUFFER_SIZE, "%s%s", URL_REQUEST, encoded_data);

    EXAMPLE_HTTP_REQUEST_T req = {0};
    req.hostname = HOST;
    req.url = full_url;
    req.tls_config = altcp_tls_create_config_client(NULL, 0);
    req.headers_fn = http_client_header_print_fn;
    req.recv_fn = http_receive_cb;

    printf("Enviando: %s\n", data);
    int result = http_client_request_sync(cyw43_arch_async_context(), &req);

    altcp_tls_free_config(req.tls_config);
    if (result != 0)
    {
        printf("Erro ao enviar! Código: %d\n", result);
    }
}

// Configura UART
void setup_uart()
{
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_fifo_enabled(UART_ID, false);
}

// Processa sentença GPGGA e envia ao servidor + salva no SD
void process_gpgga(char *sentence)
{
    char *token;
    char *data[15];
    int i = 0;

    token = strtok(sentence, ",");
    while (token != NULL && i < 15)
    {
        data[i++] = token;
        token = strtok(NULL, ",");
    }

    if (i < 6 || data[2] == NULL || data[4] == NULL)
    {
        printf("Sentença GPS incompleta.\n");
        return;
    }

    double lat_raw = atof(data[2]);
    double lat_deg = (int)(lat_raw / 100);
    double lat_min = lat_raw - (lat_deg * 100);
    double latitude = lat_deg + lat_min / 60.0;
    if (data[3][0] == 'S')
        latitude *= -1;

    double lon_raw = atof(data[4]);
    double lon_deg = (int)(lon_raw / 100);
    double lon_min = lon_raw - (lon_deg * 100);
    double longitude = lon_deg + lon_min / 60.0;
    if (data[5][0] == 'W')
        longitude *= -1;

    char msg[BUFFER_SIZE];
    snprintf(msg, BUFFER_SIZE, "placa=LUV123, latitude=%.6f, longitude=%.6f", latitude, longitude);

    // Envia pro servidor
    send_data(msg);

    // Salva no SD
    salvar_no_sd(msg);
    salvar_no_sd("\n");
}

// Lê continuamente da UART e processa GPGGA
void read_gps_loop()
{
    int idx = 0;
    while (true)
    {
        verificar_botao();
        atualizar_leds();
        if (uart_is_readable(UART_ID))
        {
            char c = uart_getc(UART_ID);

            if (c == '\n' || idx >= BUFFER_SIZE - 1)
            {
                gps_buffer[idx] = '\0';
                idx = 0;

                if (strstr(gps_buffer, "$GPGGA"))
                {
                    process_gpgga(gps_buffer);
                }
            }
            else
            {
                gps_buffer[idx++] = c;
            }
        }
    }
}

int main()
{
    stdio_init_all();
    sleep_ms(200);
    printf("Iniciando...\n");
    gpio_init(LED_ON_PIN);
    gpio_set_dir(LED_ON_PIN, GPIO_OUT);
    gpio_put(LED_ON_PIN, 0);

    gpio_init(LED_OFF_PIN);
    gpio_set_dir(LED_OFF_PIN, GPIO_OUT);
    gpio_put(LED_OFF_PIN, 0);

    // Configura botão
    gpio_init(BUTTON_A);
    gpio_set_dir(BUTTON_A, GPIO_IN);
    gpio_pull_up(BUTTON_A); // botão para GND

    setup_uart();

    // Inicializa Wi-Fi
    if (cyw43_arch_init())
    {
        printf("Falha ao iniciar Wi-Fi\n");
        return 1;
    }

    cyw43_arch_enable_sta_mode();
    if (cyw43_arch_wifi_connect_timeout_ms("Lapec_Professores", "w1q2e3r4", CYW43_AUTH_WPA2_AES_PSK, 10000))
    {
        printf("Erro ao conectar no Wi-Fi\n");
        return 1;
    }
    printf("Wi-Fi conectado com sucesso!\n");

    // Inicializa SD
    init_sd();

    // Loop principal GPS
    read_gps_loop();

    cyw43_arch_deinit();
    return 0;
}
