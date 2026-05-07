#!/bin/bash

# Renkli çıktı için
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${GREEN}[*] Hypervisor Hazirlik Scripti Baslatiliyor...${NC}"

# 1. ZRAM ve Swap Kapatma
if lsblk | grep -q "zram0"; then
    echo -e "${GREEN}[+] ZRAM Swap kapatiliyor...${NC}"
    sudo swapoff /dev/zram0 2>/dev/null
    sudo zramctl -r /dev/zram0 2>/dev/null
else
    echo -e "[!] Aktif ZRAM bulunamadi."
fi

# 2. KVM Unload
# Not: KVM modulleri genelde baska surecler tarafindan (libvirt vb.) kullaniliyorsa rmmod hata verir
# Bu yuzden modprobe -r daha saglikli
echo -e "${GREEN}[+] KVM ve AMD-V modulleri kaldiriliyor...${NC}"
sudo modprobe -r kvm_amd
sudo modprobe -r kvm

# 3. ZRAM Unload
echo -e "${GREEN}[+] ZRAM modulu temizleniyor...${NC}"
sudo modprobe -r zram

# 4. Kontrol
echo -e "\n${GREEN}[*] Mevcut Durum:${NC}"
echo -ne "KVM AMD: "
lsmod | grep -q kvm_amd && echo -e "${RED}YUKLU${NC}" || echo -e "${GREEN}KALDIRILDI${NC}"
echo -ne "ZRAM:    "
lsmod | grep -q zram && echo -e "${RED}YUKLU${NC}" || echo -e "${GREEN}KALDIRILDI${NC}"

echo -e "\n${GREEN}[!] Sistem artik svm-core icin temiz ${NC}"
