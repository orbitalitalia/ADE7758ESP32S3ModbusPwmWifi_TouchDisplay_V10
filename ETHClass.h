#ifndef _ETH_H_
#define _ETH_H_

#include "WiFi.h"
#include "esp_system.h"
#include "esp_eth.h"
#include "driver/spi_master.h"

#ifndef ETH_PHY_ADDR
#define ETH_PHY_ADDR 0
#endif

#ifndef ETH_PHY_TYPE
#define ETH_PHY_TYPE ETH_PHY_LAN8720
#endif

#ifndef ETH_PHY_POWER
#define ETH_PHY_POWER -1
#endif

#ifndef ETH_PHY_MDC
#define ETH_PHY_MDC 23
#endif

#ifndef ETH_PHY_MDIO
#define ETH_PHY_MDIO 18
#endif

#ifndef ETH_CLK_MODE
#define ETH_CLK_MODE ETH_CLOCK_GPIO0_IN
#endif

#if ESP_IDF_VERSION_MAJOR > 3
typedef enum { ETH_CLOCK_GPIO0_IN, ETH_CLOCK_GPIO0_OUT, ETH_CLOCK_GPIO16_OUT, ETH_CLOCK_GPIO17_OUT } eth_clock_mode_t;
#endif

typedef enum { ETH_PHY_LAN8720, ETH_PHY_TLK110, ETH_PHY_RTL8201, ETH_PHY_DP83848, ETH_PHY_DM9051, ETH_PHY_KSZ8041, ETH_PHY_KSZ8081, ETH_PHY_MAX } eth_phy_type_t;
#define ETH_PHY_IP101 ETH_PHY_TLK110

class ETHClass
{
private:
    bool initialized;
    bool staticIP;
#if ESP_IDF_VERSION_MAJOR > 3
    esp_eth_handle_t eth_handle;
    esp_netif_t *eth_netif;
protected:
    bool started;
    static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
#else
    bool started;
    eth_config_t eth_config;
#endif
public:
    ETHClass();
    ~ETHClass();

    bool beginSPI( int miso, int mosi, int sck, int cs, int rst, int irq,
                   spi_host_device_t host_id = SPI3_HOST,
                   uint8_t phy_addr = ETH_PHY_ADDR,
                   int clk_mhz = 36,
                   bool use_mac_from_efuse = false);

    bool begin(uint8_t phy_addr = ETH_PHY_ADDR, int power = ETH_PHY_POWER, int mdc = ETH_PHY_MDC, int mdio = ETH_PHY_MDIO, eth_phy_type_t type = ETH_PHY_TYPE, eth_clock_mode_t clk_mode = ETH_CLK_MODE, bool use_mac_from_efuse = false);

    bool config(IPAddress local_ip, IPAddress gateway, IPAddress subnet, IPAddress dns1 = (uint32_t)0x00000000, IPAddress dns2 = (uint32_t)0x00000000);

    const char *getHostname();
    bool setHostname(const char *hostname);

    bool fullDuplex();
    bool linkUp();
    uint8_t linkSpeed();

    bool enableIpV6();
    IPv6Address localIPv6();

    IPAddress localIP();
    IPAddress subnetMask();
    IPAddress gatewayIP();
    IPAddress dnsIP(uint8_t dns_no = 0);

    IPAddress broadcastIP();
    IPAddress networkID();
    uint8_t subnetCIDR();

    uint8_t *macAddress(uint8_t *mac);
    String macAddress();

    friend class WiFiClient;
    friend class WiFiServer;
};

extern ETHClass ETH;

#endif /* _ETH_H_ */