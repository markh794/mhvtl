#!/bin/sh

###  Install script for auto install mhvtl on CentOS Linux 6.2 i386/x86_64
###  Version: 1.0.0
###  made by patrick ru
###  mail: patrick.ru@hotmail.com
###  Date: 2012.Mar.18

# prepair : pre-install centos linux 6.2 with minimal installation.

# update the system
yum update -y

# close the selinux and the firewall
chkconfig iptables off
service iptables stop
sed -i 's/SELINUX=enforcing/SELINUX=disabled/g' /etc/selinux/config
/usr/sbin/setenforce 0

# install Git
yum install -y git

# install supported Rpms
yum install -y mc ntp gcc gcc-c++ make kernel-devel zlib-devel sg3_utils lsscsi mt-st mtx lzo lzo-devel perl-Config-General

# create user, group and folders
/usr/sbin/groupadd -r vtl
/usr/sbin/useradd -r -c "Virtual Tape Library" -d /opt/mhvtl -g vtl vtl
mkdir -p /opt/mhvtl
mkdir -p /etc/mhvtl
chown -Rf vtl:vtl /opt/mhvtl
chown -Rf vtl:vtl /etc/mhvtl

# install mhvtl
mkdir -p /usr/src/mhvtl
cd /usr/src/mhvtl
git init
git pull http://github.com/markh794/mhvtl.git 
make distclean
cd kernel/
make && make install
cd ..
make && make install
chkconfig --add mhvtl
chkconfig mhvtl on
/etc/init.d/mhvtl start

# install tgt
mkdir /etc/tgt
mkdir /usr/src/tgt
cd /usr/src/tgt
git init
git pull http://github.com/fujita/tgt.git
make && make install
/usr/sbin/tgtd -d 1

# install mhvtl-gui
yum install -y httpd php sudo sysstat
cp /etc/sudoers /etc/sudoers.old
sed -i '/Defaults    requiretty/s/^/#/' /etc/sudoers
echo "apache ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers
chkconfig httpd on
mkdir /var/www/html/mhvtl
cd /var/www/html/mhvtl
git init
git pull http://github.com/niadev67/mhvtl-gui.git
chown -R apache:apache ./
service httpd start
touch /var/www/html/mhvtl/ENABLE_TGTD_SCSI_TARGET
sh scripts/auto.iscsi.config.stgt.sh

echo installation has finished! please use mhvtl-gui to install and config tgtd after config the mhvtl for running!
