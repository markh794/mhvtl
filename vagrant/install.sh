#!/bin/bash

# This script assumes it is located in a subdirectory from 'mhvtl' source root

echo "Script Begin"

get_os_name()
{
	if [ "$(hostnamectl | grep -i ubuntu | wc -l)" != "0" ]; then
		OS_NAME='ubuntu'
	elif [ "$(hostnamectl | grep -i sles | wc -l)" != "0" ]; then
		OS_NAME='sles'
	elif [ "$(hostnamectl | grep -i opensuse | wc -l)" != "0" ]; then
		OS_NAME='opensuse'
	elif [ "$(hostnamectl | grep -i centos | wc -l)" != "0" ]; then
		OS_NAME='centos'
	elif [ "$(hostnamectl | grep -i rocky | wc -l)" != "0" ]; then
		OS_NAME='rockylinux'
	elif [ "$(hostnamectl | grep -i alma | wc -l)" != "0" ]; then
		OS_NAME='almalinux'
	else
		echo 'This os is not supported!'
		exit 1
	fi
	echo "OS_NAME is $OS_NAME"
}

# check our script has been started with root auth
if [ "$(id -u)" != "0" ]; then
	echo "This script must be run with root privileges. Please run again as either root or using sudo."
	tput sgr0
	exit 1
fi

get_os_name

# Lets break the script if there are any errors
set -e

install_ubuntu_pre_req()
{
	echo "Ubuntu"
	sudo apt-get update && sudo apt-get install ntp sysstat mtx mt-st sg3-utils zlib1g-dev git lsscsi build-essential gawk alien fakeroot linux-headers-$(uname -r) linux-modules-extra-$(uname -r) targetcli-fb -y
}

install_alma_pre_req()
{
	echo "alma"

#	sudo dnf install -y deltarpm
	sudo dnf update -y && sudo yum install -y git mc gcc gcc-c++ make kernel-devel-$(uname -r) zlib-devel sg3_utils lsscsi mt-st mtx targetcli vim chrony policycoreutils-python-utils policycoreutils
	sudo dnf upgrade -y
	# Rebuild VBox guest tools for any new kernel(s) installed
#	/sbin/rcvboxadd quicksetup all
}

install_rocky_pre_req()
{
	echo "Rocky"

#	sudo yum install -y deltarpm
	sudo yum update -y && sudo yum install -y git mc gcc gcc-c++ make kernel-devel-$(uname -r) zlib-devel sg3_utils lsscsi mt-st mtx targetcli vim policycoreutils-python-utils policycoreutils
	sudo yum upgrade -y
	# Rebuild VBox guest tools for any new kernel(s) installed
	/sbin/rcvboxadd quicksetup all
}

install_centos_pre_req()
{
	echo "CentOS"

	sed -i 's/mirrorlist/#mirrorlist/g' /etc/yum.repos.d/CentOS-*
	sed -i 's|#baseurl=http://mirror.centos.org|baseurl=http://vault.centos.org|g' /etc/yum.repos.d/CentOS-*

	sudo yum install -y deltarpm
	sudo yum update -y && sudo yum install -y git mc ntp gcc gcc-c++ make kernel-devel-$(uname -r) zlib-devel sg3_utils lsscsi mt-st mtx perl-Config-General targetcli policycoreutils-python-utils policycoreutils
	sudo yum upgrade -y
	# Rebuild VBox guest tools for any new kernel(s) installed
	/sbin/rcvboxadd quicksetup all

}

install_sles_pre_req()
{
	echo "SLES/OpenSuse"

	# Workaround so that we install the same kernel-devel and kernel-syms version as the running kernel.
	UNAME_R=$(echo $(uname -r) | cut -d "-" -f-2)
	PATCHED_KERNEL_VERSION=$(sudo zypper se -s kernel-devel | grep ${UNAME_R} | cut -d "|" -f4 | tr -d " ")
	sudo zypper install -y --oldpackage kernel-devel-${PATCHED_KERNEL_VERSION}
	sudo zypper install -y --oldpackage kernel-syms-${PATCHED_KERNEL_VERSION}

	sudo zypper install -y git mc ntp gcc gcc-c++ make zlib-devel sg3_utils lsscsi mtx perl-Config-General targetcli-fb python-semanage
}

install_pre_req()
{
	if [ ${OS_NAME} == 'ubuntu' ]; then
		SYSTEMD_GENERATOR_DIR="/lib/systemd/system-generators"
		install_ubuntu_pre_req

	elif [ ${OS_NAME} == 'rockylinux' ]; then
		SYSTEMD_GENERATOR_DIR="/lib/systemd/system-generators"
		install_rocky_pre_req

	elif [ ${OS_NAME} == 'centos' ]; then
		SYSTEMD_GENERATOR_DIR="/lib/systemd/system-generators"
		install_centos_pre_req

	elif [ ${OS_NAME} == 'almalinux' ]; then
		SYSTEMD_GENERATOR_DIR="/usr/lib/systemd/system-generators"
		install_alma_pre_req

	elif [ ${OS_NAME} == 'sles' ] || [ ${OS_NAME} == 'opensuse' ]; then
		SYSTEMD_GENERATOR_DIR="/usr/lib/systemd/system-generators"
		install_sles_pre_req
	else
		echo "Unable to handle install_pre_req for ${OS_NAME}"
	fi
}

install_mhvtl_kernel_module()
{
	make distclean
	if [ ${OS_NAME} == 'ubuntu' ]; then
		make
		sudo make install
	elif [ ${OS_NAME} == 'centos' ] || [ ${OS_NAME} == 'rockylinux' ] || [ ${OS_NAME} == 'almalinux' ]; then
		for a in `rpm -qa | awk '/kernel-devel/ {print $1}' | sed -e "s/kernel-devel-//g"`
		do
			echo "Building mhVTL kernel module for ${a}"
			make V=${a}
			sudo make install V=${a}
			make distclean
		done
	elif [ ${OS_NAME} == 'sles' ] || [ ${OS_NAME} == 'opensuse' ]; then
		make
		sudo make install
	fi
}

setup_time()
{
	echo "Setting timezone to $1"
	timedatectl set-timezone $1

	if [ ${OS_NAME} == 'almalinux' ]; then
		echo "Alma linux - ntp (timesyncd) enabled by default"
		systemctl restart chronyd
		return
	fi
	if [ ${OS_NAME} == 'rockylinux' ]; then
		echo "Rocky linux - ntp (timesyncd) enabled by default"
		systemctl restart chronyd
		return
	fi
	# And enable ntp.
	if [ $(egrep "^server|^pool" /etc/ntp.conf | wc -l) -eq 0 ]; then
		rcntpd addserver 0.au.pool.ntp.org
		rcntpd addserver 1.au.pool.ntp.org
	fi
	if [ ${OS_NAME} == 'ubuntu' ]; then
		NTP="ntp"
	elif [ ${OS_NAME} == 'almalinux' ]; then
		NTP="chrony"
	elif [ ${OS_NAME} == 'rockylinux' ]; then
		NTP="chrony"
	elif [ ${OS_NAME} == 'centos' ]; then
		NTP="ntpd"
	elif [ ${OS_NAME} == 'sles' ] || [ ${OS_NAME} == 'opensuse' ]; then
		NTP="ntpd"
	else
		echo 'Could not determine os type'
		return
	fi
	systemctl enable ${NTP}
	systemctl start ${NTP}
	# Rocky Linux - ntp (chrony) is already running, so restart
	systemctl restart ${NTP}
}

# Install required packages
install_pre_req

# Use 'timedatectl list-timezones' to view valid timezone strings
setup_time "Australia/Sydney"

# Change to mhVTL folder
cd ../

# Clean up any previous build
make distclean

# Build kernel module for all versions which have the development package installed
cd kernel/
install_mhvtl_kernel_module

# Now make user-space binaries and install
cd ..
make
echo "placing SYSTEMD_GENERATOR_DIR : ${SYSTEMD_GENERATOR_DIR}"
sudo make install SYSTEMD_GENERATOR_DIR=${SYSTEMD_GENERATOR_DIR}

# Load it
sudo depmod -a
sudo systemctl daemon-reload
sudo systemctl enable mhvtl.target
sudo systemctl start mhvtl.target

sleep 3
echo "Show your tape libraries now!"
hba=`lsscsi -H | awk '/mhvtl/ {print $1}' | sed -e 's/\[//g' -e 's/\]//g'`
lsscsi ${hba} -g

echo ""
if [ "$(lsscsi -g ${hba} | wc -l)" -gt 2 ]; then
	echo "Found some virtual tapes, success!"
else
	echo "Could not find the virtual tapes, the installation failed!"
	exit 1
fi

exit 0
