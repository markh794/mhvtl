#!/bin/bash
echo "Script Begin"

get_os_name(){
    if [[ "$(hostnamectl | grep -i ubuntu | wc -l)" != "0" ]]; then
        OS_NAME='ubuntu'
    elif [[ "$(hostnamectl | grep -i sles | wc -l)" != "0" ]]; then
        OS_NAME='sles'
    elif [[ "$(hostnamectl | grep -i opensuse | wc -l)" != "0" ]]; then
	OS_NAME='opensuse'
    elif [[ "$(hostnamectl | grep -i centos | wc -l)" != "0" ]]; then
        OS_NAME='centos'
    else
        echo 'This os is not supported!'
        exit 1
    fi
    echo "OS_NAME is $OS_NAME"
}

# check our script has been started with root auth
if [[ "$(id -u)" != "0" ]]; then
	echo "This script must be run with root privileges. Please run again as either root or using sudo."
	tput sgr0
	exit 1
fi

get_os_name

# Lets break the script if there are any errors
set -e

install_ubuntu_pre_req(){
    sudo apt-get update && sudo apt-get install sysstat lzop liblzo2-dev liblzo2-2 mtx mt-st sg3-utils zlib1g-dev git lsscsi build-essential gawk alien fakeroot linux-headers-$(uname -r) -y
}
install_centos_pre_req(){
    sudo yum update -y && sudo yum install -y git mc ntp gcc gcc-c++ make kernel-devel zlib-devel sg3_utils lsscsi mt-st mtx lzo lzo-devel perl-Config-General
}
install_sles_pre_req(){
    echo "SLES/OpenSuse IS NOT YET SUPPORTED! Use it at your own risk!"

    # Workaround so that we install the same kernel-devel and kernel-syms version as the running kernel.
    UNAME_R=$(echo $(uname -r) | cut -d "-" -f-2)
    PATCHED_KERNEL_VERSION=$(sudo zypper se -s kernel-devel | grep ${UNAME_R} | cut -d "|" -f4 | tr -d " ")
    sudo zypper install -y --oldpackage kernel-devel-${PATCHED_KERNEL_VERSION}
    sudo zypper install -y --oldpackage kernel-syms-${PATCHED_KERNEL_VERSION}

    sudo zypper install -y git mc ntp gcc gcc-c++ make zlib-devel sg3_utils lsscsi mtx lzo lzo-devel perl-Config-General
}

install_pre_req(){
    if [[ ${OS_NAME} == 'ubuntu' ]]; then
        install_ubuntu_pre_req
    elif [[ ${OS_NAME} == 'centos' ]]; then
        install_centos_pre_req
    elif [[ ${OS_NAME} == 'sles' ]] || [[ ${OS_NAME} == 'opensuse' ]]; then
        install_sles_pre_req
    fi
}

# Install required packages
install_pre_req

# Change to mhVTL folder
cd ../
make distclean
cd kernel/
make
sudo make install
cd ..
make
sudo make install

# Load it
sudo systemctl daemon-reload
sudo systemctl enable mhvtl.target
sudo systemctl start mhvtl.target

sleep 3
echo "Show your tape libraries now!"
lsscsi
