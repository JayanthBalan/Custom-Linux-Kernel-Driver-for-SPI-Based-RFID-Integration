#!/bin/bash

git submodule init
git submodule sync
git submodule update

source poky/oe-init-build-env

if [[ $1 == "-bbb" || $2 == "-bbb" || $3 == "-bbb" || $4 == "-bbb" ]]; then
	CONFLINE="MACHINE ?= \"beaglebone-yocto\""
else
	CONFLINE="MACHINE ?= \"qemuarm64\""
fi

cat conf/local.conf | grep "${CONFLINE}" > /dev/null
local_conf_info=$?

if [ $local_conf_info -ne 0 ];then
	echo "Append ${CONFLINE} in the local.conf file"
	echo ${CONFLINE} >> conf/local.conf
	
else
	echo "${CONFLINE} already exists in the local.conf file"
fi


if [[ $1 == "-devdrive" || $2 == "-devdrive" || $3 == "-devdrive" || $4 == "-devdrive" ]]; then
	bitbake-layers show-layers | grep "meta-devdrive" > /dev/null
	layer_info=$?
	if [ $layer_info -ne 0 ];then
		echo "Adding meta-devdrive layer"
		bitbake-layers add-layer ../meta-devdrive
	else
		echo "meta-devdrive layer already exists"
	fi
fi

if [[ $1 == "-authapp" || $2 == "-authapp" || $3 == "-authapp" || $4 == "-authapp" ]]; then
	bitbake-layers show-layers | grep "meta-authapp" > /dev/null
	layer_info=$?
	if [ $layer_info -ne 0 ];then
		echo "Adding meta-authapp layer"
		bitbake-layers add-layer ../meta-authapp
	else
		echo "meta-authapp layer already exists"
	fi
fi

set -e

if [[ $1 == "-minimal" || $2 == "-minimal" || $3 == "-minimal" || $4 == "-minimal" ]]; then
	bitbake core-image-minimal
else
	bitbake core-image-aeldfinal
fi
