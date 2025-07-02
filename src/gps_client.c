#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "pico/cyw43_arch.h"
#include "lwip/altcp_tls.h"
#include "example_http_client_util.h"

#define UART_ID uart1
#define BAUD_RATE 9600
#define UART_TX_PIN 8
#define UART_RX_PIN 9

#define HOST "server-findway.onrender.com"
#define URL_REQUEST "/mensagem?msg="
#define BUFFER_SIZE 512

char gps_buffer[BUFFER_SIZE];

// Codifica string para URL
void urlencode(const char *input, char *output, size_t output_size) {
    char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; input[i] && j + 3 < output_size; i++) {
        if ((input[i] >= 'A' && input[i] <= 'Z') || 
            (input[i] >= 'a' && input[i] <= 'z') || 
            (input[i] >= '0' && input[i] <= '9') || 
            input[i] == '-' || input[i] == '_' || input[i] == '.' || input[i] == '~') {
            output[j++] = input[i];
        } else {
            output[j++] = '%';
            output[j++] = hex[(input[i] >> 4) & 0xF];
            output[j++] = hex[input[i] & 0xF];
        }
    }
    output[j] = '\0';
}

// Envia dados para o servidor
void send_data(const char* data) {
    char encoded_data[BUFFER_SIZE];
    urlencode(data, encoded_data, BUFFER_SIZE);

    char full_url[BUFFER_SIZE];
    snprintf(full_url, BUFFER_SIZE, "%s%s", URL_REQUEST, encoded_data);

    EXAMPLE_HTTP_REQUEST_T req = {0};
    req.hostname = HOST;
    req.url = full_url;
    req.tls_config = altcp_tls_create_config_client(NULL, 0);
    req.headers_fn = http_client_header_print_fn;
    req.recv_fn = http_client_receive_print_fn;

    printf("Enviando: %s\n", data);
    int result = http_client_request_sync(cyw43_arch_async_context(), &req);

    altcp_tls_free_config(req.tls_config);
    if (result != 0) {
        printf("Erro ao enviar! Código: %d\n", result);
    }
}

// Configura UART
void setup_uart() {
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_fifo_enabled(UART_ID, false);
}

// Processa sentença GPGGA e envia ao servidor
void process_gpgga(char *sentence) {
    char *token;
    char *data[15];
    int i = 0;

    token = strtok(sentence, ",");
    while (token != NULL && i < 15) {
        data[i++] = token;
        token = strtok(NULL, ",");
    }

    if (i < 6 || data[2] == NULL || data[4] == NULL) {
        printf("Sentença GPS incompleta.\n");
        return;
    }

    double lat_raw = atof(data[2]);
    double lat_deg = (int)(lat_raw / 100);
    double lat_min = lat_raw - (lat_deg * 100);
    double latitude = lat_deg + lat_min / 60.0;
    if (data[3][0] == 'S') latitude *= -1;

    double lon_raw = atof(data[4]);
    double lon_deg = (int)(lon_raw / 100);
    double lon_min = lon_raw - (lon_deg * 100);
    double longitude = lon_deg + lon_min / 60.0;
    if (data[5][0] == 'W') longitude *= -1;

    char msg[BUFFER_SIZE];
    snprintf(msg, BUFFER_SIZE, "Localização GPS: latitude=%.6f, longitude=%.6f", latitude, longitude);

    send_data(msg);
}

// Lê continuamente da UART e processa GPGGA
void read_gps_loop() {
    int idx = 0;
    while (true) {
        if (uart_is_readable(UART_ID)) {
            char c = uart_getc(UART_ID);            

            if (c == '\n' || idx >= BUFFER_SIZE - 1) {
                gps_buffer[idx] = '\0';
                idx = 0;

                if (strstr(gps_buffer, "$GPGGA")) {
                    process_gpgga(gps_buffer);
                }
            } else {
                gps_buffer[idx++] = c;
            }
        }
    }
}

int main() {
    stdio_init_all();
    sleep_ms(200);
    printf("Iniciando...\n");

    setup_uart();

    if (cyw43_arch_init()) {
        printf("Falha ao iniciar Wi-Fi\n");
        return 1;
    }

    cyw43_arch_enable_sta_mode();
    if (cyw43_arch_wifi_connect_timeout_ms("POCO", "12345678", CYW43_AUTH_WPA2_AES_PSK, 10000)) {
        printf("Erro ao conectar no Wi-Fi\n");
        return 1;
    }

    printf("Wi-Fi conectado com sucesso!\n");
    read_gps_loop();

    cyw43_arch_deinit();
    return 0;
}