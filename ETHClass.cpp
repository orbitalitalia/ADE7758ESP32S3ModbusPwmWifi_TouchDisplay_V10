#include "ETHClass.h"
#include "esp_system.h"
#if ESP_IDF_VERSION_MAJOR > 3
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_eth_phy.h"
#include "esp_eth_mac.h"
#include "esp_eth_com.h"
#if CONFIG_IDF_TARGET_ESP32
#include "soc/emac_ext_struct.h"
#include "soc/rtc.h"
#endif
#endif
#include "lwip/err.h"
#include "lwip/dns.h"

extern void tcpipInit();
extern void add_esp_interface_netif(esp_interface_t interface, esp_netif_t *esp_netif);

#if ESP_IDF_VERSION_MAJOR > 3
static eth_clock_mode_t eth_clock_mode = ETH_CLK_MODE;
#endif

ETHClass::ETHClass()
    : initialized(false)
    , staticIP(false)
#if ESP_IDF_VERSION_MAJOR > 3
    , eth_handle(NULL)
    , eth_netif(NULL)
#endif
    , started(false)
{
}

ETHClass::~ETHClass()
{}

bool ETHClass::beginSPI( int miso, int mosi, int sck, int cs, int rst, int irq,
                         spi_host_device_t host_id,
                         uint8_t phy_addr,
                         int clk_mhz,
                         bool use_mac_from_efuse)
{
#if ESP_IDF_VERSION_MAJOR > 3
    esp_err_t   err;
    tcpipInit();
    tcpip_adapter_set_default_eth_handlers();

    if (rst > -1) {
        pinMode(rst, OUTPUT);
        digitalWrite(rst, HIGH);
        delay(250);
        digitalWrite(rst, LOW);
        delay(50);
        digitalWrite(rst, HIGH);
        delay(350);
    }

    if (use_mac_from_efuse) {
        uint8_t p[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        esp_efuse_mac_get_custom(p);
        esp_base_mac_addr_set(p);
    }

    esp_eth_mac_t *eth_mac = NULL;
    esp_eth_phy_t *eth_phy = NULL;

    // Create instance(s) of esp-netif for SPI Ethernet(s)
    esp_netif_inherent_config_t esp_netif_config = ESP_NETIF_INHERENT_DEFAULT_ETH();

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    cfg.base = &esp_netif_config;
    cfg.stack = ESP_NETIF_NETSTACK_DEFAULT_ETH;

    char if_key_str[10] = "ETH_SPI_0";
    char if_desc_str[10] = "eth0";
    esp_netif_config.if_key = if_key_str;
    esp_netif_config.if_desc = if_desc_str;
    esp_netif_config.route_prio = 30;
    eth_netif = esp_netif_new(&cfg);

    // Init MAC and PHY configs to default
    eth_mac_config_t mac_config_spi = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

    // Install GPIO ISR handler to be able to service SPI Eth modlues interrupts
    gpio_install_isr_service(0);

    // Init SPI bus
    spi_device_handle_t spi_handle = {0};
    spi_bus_config_t buscfg  = {0};
    buscfg.miso_io_num = miso;
    buscfg.mosi_io_num = mosi;
    buscfg.sclk_io_num = sck;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    err = spi_bus_initialize(host_id, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        log_e("spi_bus_initialize failed");
        return false;
    }

    spi_device_interface_config_t devcfg = {
        .command_bits = 16,
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = clk_mhz * 1000 * 1000,
        .queue_size = 20
    };

    devcfg.spics_io_num = cs;

    err = spi_bus_add_device(host_id, &devcfg, &spi_handle);
    if (err != ESP_OK) {
        log_e("spi_bus_add_device failed");
        return false;
    }

    // w5500 ethernet driver is based on spi driver
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(spi_handle);

    w5500_config.int_gpio_num = irq;
    phy_config.phy_addr = phy_addr;
    phy_config.reset_gpio_num = rst;

    eth_mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config_spi);
    if (eth_mac == NULL) {
        log_e("esp_eth_mac_new_esp32 failed");
        return false;
    }

    eth_phy = esp_eth_phy_new_w5500(&phy_config);
    if (eth_phy == NULL) {
        log_e("esp_eth_phy_new failed");
        return false;
    }

    eth_handle = NULL;
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(eth_mac, eth_phy);
    if (esp_eth_driver_install(&eth_config, &eth_handle) != ESP_OK || eth_handle == NULL) {
        log_e("esp_eth_driver_install failed");
        return false;
    }

    uint8_t  address[] = {
        0x02, 0x00, 0x00, 0x12, 0x34, 0x56
    };
    esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, address );

    if (esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)) != ESP_OK) {
        log_e("esp_netif_attach failed");
        return false;
    }

    add_esp_interface_netif(ESP_IF_ETH, eth_netif);

    if (esp_eth_start(eth_handle) != ESP_OK) {
        log_e("esp_eth_start failed");
        return false;
    }
#endif
    delay(50);
    return true;
}

bool ETHClass::config(IPAddress local_ip, IPAddress gateway, IPAddress subnet, IPAddress dns1, IPAddress dns2)
{
    extern esp_err_t set_esp_interface_ip(esp_interface_t interface, IPAddress local_ip, IPAddress gateway, IPAddress subnet, IPAddress dhcp_lease_start = INADDR_NONE);
    set_esp_interface_ip(ESP_IF_ETH, local_ip, gateway, subnet);
    return true;
}

IPAddress ETHClass::localIP()
{
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(eth_netif, &ip) == ESP_OK) {
        return IPAddress(ip.ip.addr);
    }
    return IPAddress();
}

IPAddress ETHClass::subnetMask()
{
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(eth_netif, &ip) == ESP_OK) {
        return IPAddress(ip.netmask.addr);
    }
    return IPAddress();
}

IPAddress ETHClass::gatewayIP()
{
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(eth_netif, &ip) == ESP_OK) {
        return IPAddress(ip.gw.addr);
    }
    return IPAddress();
}

IPAddress ETHClass::dnsIP(uint8_t dns_no)
{
    const ip_addr_t *dns_ip = dns_getserver(dns_no);
    return IPAddress(dns_ip->u_addr.ip4.addr);
}

const char *ETHClass::getHostname()
{
    const char *hostname;
    if (tcpip_adapter_get_hostname(TCPIP_ADAPTER_IF_ETH, &hostname)) {
        return NULL;
    }
    return hostname;
}

bool ETHClass::setHostname(const char *hostname)
{
    return tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_ETH, hostname) == 0;
}

bool ETHClass::fullDuplex()
{
#if ESP_IDF_VERSION_MAJOR > 3
    eth_duplex_t link_duplex;
    esp_eth_ioctl(eth_handle, ETH_CMD_G_DUPLEX_MODE, &link_duplex);
    return (link_duplex == ETH_DUPLEX_FULL);
#else
    return eth_config.phy_get_duplex_mode();
#endif
}

bool ETHClass::linkUp()
{
#if ESP_IDF_VERSION_MAJOR > 3
    return WiFiGenericClass::getStatusBits() & ETH_CONNECTED_BIT;
#else
    return eth_config.phy_check_link();
#endif
}

uint8_t ETHClass::linkSpeed()
{
#if ESP_IDF_VERSION_MAJOR > 3
    eth_speed_t link_speed;
    esp_eth_ioctl(eth_handle, ETH_CMD_G_SPEED, &link_speed);
    return (link_speed == ETH_SPEED_10M) ? 10 : 100;
#else
    return eth_config.phy_get_speed_mode() ? 100 : 10;
#endif
}

uint8_t *ETHClass::macAddress(uint8_t *mac)
{
    if (!mac) {
        return NULL;
    }
#ifdef ESP_IDF_VERSION_MAJOR
    esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac);
#else
    esp_eth_get_mac(mac);
#endif
    return mac;
}

String ETHClass::macAddress(void)
{
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
    char macStr[18] = { 0 };
    macAddress(mac);
    sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
}