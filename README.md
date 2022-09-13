## Linux driver for MACPHY

#compile : 
apt get install libncurses-dev gawk flex bison openssl libssl-dev dkms libelf-dev libudev-dev libpci-dev libiberty-dev autoconf raspberrypi-kernel-headers 
make
make device
make load

#samba:
apt get install samba samba-common-bin
smbpasswd -a user
nano /etc/samba/smb.conf

[pihome]
    comment = Pi Home
    path = /home/pi
    browseable = yes
    writeable = yes
    force create mode = 0777
    force directory mode = 0777
    public = no

#Net :
apt get install iperf3
ifconfig ncv7410-0 192.168.5.5/24
ip route add 192.168.5.0/24 dev ncv7410-0
iperf3 -s
iperf3 -c 192.168.0.48