#!/bin/bash

git submodule init
git submodule sync
git submodule update

source poky/oe-init-build-env "$PWD/build"

CONFLINE_bbb="MACHINE = \"beaglebone-yocto\""
CONFLINE_qemu="MACHINE = \"qemuarm64\""
if [[ $1 == "-bbb" || $2 == "-bbb" || $3 == "-bbb" || $4 == "-bbb" ]]; then
	CONFLINE=$CONFLINE_bbb
elif [[ $1 == "-qemu" || $2 == "-qemu" || $3 == "-qemu" || $4 == "-qemu" ]]; then
	CONFLINE=$CONFLINE_qemu
fi

if [[ -v CONFLINE ]]; then
	cat conf/local.conf | grep "${CONFLINE}" > /dev/null
	local_conf_info=$?
	
	if [ $local_conf_info -eq 0 ]; then
		echo "${CONFLINE} already exists in the local.conf file"	
	else
		sed -i "\|$CONFLINE_bbb|d" conf/local.conf
		sed -i "\|$CONFLINE_qemu|d" conf/local.conf

		echo "Append ${CONFLINE} in the local.conf file"
		echo ${CONFLINE} >> conf/local.conf
	fi
fi

mod_devdrive_layer() {
	bitbake-layers show-layers | grep "meta-devdrive" > /dev/null
	local layer_info=$?
	if [ $layer_info -ne 0 ];then
		echo "Adding meta-devdrive layer"
		bitbake-layers add-layer ../meta-devdrive
	else
		echo "meta-devdrive layer already exists"
	fi
}

mod_authapp_layer() {
	bitbake-layers show-layers | grep "meta-authapp" > /dev/null
	local layer_info=$?
	if [ $layer_info -ne 0 ];then
		echo "Adding meta-authapp layer"
		bitbake-layers add-layer ../meta-authapp
	else
		echo "meta-authapp layer already exists"
	fi
}

layer_opt=0
if [[ ($1 == "-devdrive" || $2 == "-devdrive" || $3 == "-devdrive" || $4 == "-devdrive") && ($1 == "-authapp" || $2 == "-authapp" || $3 == "-authapp" || $4 == "-authapp") ]]; then
	layer_opt=3;
elif [[ $1 == "-devdrive" || $2 == "-devdrive" || $3 == "-devdrive" || $4 == "-devdrive" ]]; then
	layer_opt=1
elif [[ $1 == "-authapp" || $2 == "-authapp" || $3 == "-authapp" || $4 == "-authapp" ]]; then
	layer_opt=2;
elif [[ $1 == "-clean" || $2 == "-clean" || $3 == "-clean" || $4 == "-clean" ]]; then
	layer_opt=4;
fi

if [ $layer_opt -eq 1 ]; then
	mod_devdrive_layer
elif [[ $layer_opt -eq 2 ]]; then
	mod_authapp_layer
elif [[ $layer_opt -eq 3 ]]; then
	mod_devdrive_layer
	mod_authapp_layer
elif [[ $layer_opt -eq 4 ]]; then
	bitbake-layers show-layers | grep "meta-authapp" > /dev/null
	layer_info=$?
	if [ $layer_info -eq 0 ]; then
		bitbake-layers remove-layer meta-authapp
	fi
	bitbake-layers show-layers | grep "meta-devdrive" > /dev/null
	layer_info=$?
	if [ $layer_info -eq 0 ]; then
		bitbake-layers remove-layer meta-devdrive
	fi
fi

set -e

if [[ $1 == "-minimal" || $2 == "-minimal" || $3 == "-minimal" || $4 == "-minimal" ]]; then
	bitbake core-image-minimal
else
	bitbake core-image-aeldfinal
fi
