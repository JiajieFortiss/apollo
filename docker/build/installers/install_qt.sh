#!/usr/bin/env bash

###############################################################################
# Copyright 2019 The Apollo Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
###############################################################################

# Fail on first error.
set -e

cd "$(dirname "${BASH_SOURCE[0]}")"

apt-get -y update && \
    apt-get -y install \
    libx11-xcb1 \
    libfreetype6 \
    libdbus-1-3 \
    libfontconfig1

. /tmp/installers/installer_base.sh

QT_VERSION_A=5.12
QT_VERSION_B=5.12.2
QT_VERSION_Z=$(echo "$QT_VERSION_B" | tr -d '.')

QT_INSTALLER=qt-opensource-linux-x64-${QT_VERSION_B}.run
CHECKSUM="384c833bfbccf596a00bb02bbad14b53201854c287daf2d99c23a93b8de4062a"
DOWLOAD_LINK=https://download.qt.io/archive/qt/${QT_VERSION_A}/${QT_VERSION_B}/${QT_INSTALLER}

pip3 install cuteci==1.5.0

download_if_not_cached $QT_INSTALLER $CHECKSUM $DOWLOAD_LINK
chmod +x $QT_INSTALLER

MY_DEST_DIR="/usr/local/Qt${QT_VERSION_B}"
cuteci \
    --installer "$PWD/$QT_INSTALLER" \
    install \
    --destdir="$MY_DEST_DIR" \
    --packages "qt.qt5.${QT_VERSION_Z}.gcc_64" \
    --keep-tools

# Hide qt5 version from end users
ln -s ${MY_DEST_DIR}/${QT_VERSION_B}/gcc_64 /usr/local/qt5

# clean up
rm -f ${QT_INSTALLER}
# Keep License files
rm -rf ${MY_DEST_DIR}/{Docs,Examples,Tools,dist} || true
rm -rf ${MY_DEST_DIR}/MaintenanceTool* || true
rm -rf ${MY_DEST_DIR}/{InstallationLog.txt,installer-changelog} || true
rm -rf ${MY_DEST_DIR}/{components,network}.xml || true

pip3 uninstall -y cuteci
apt-get clean && \
    rm -rf /var/lib/apt/lists/*

info "Relink library symlinks..."

PREV_LIBDIR="/usr/lib/x86_64-linux-gnu"
CURR_LIBDIR="${MY_DEST_DIR}/${QT_VERSION_B}/gcc_64/lib"

#for symlink in $(find "${PREV_LIBDIR}" -name "libQt5*.so.5" -type l); do
#    info "Removing existing $symlink"
#    unlink $symlink
#done
#for symlink in $(find "${CURR_LIBDIR}" -name "libQt5*.so.5" -type l); do
#    info "Found $symlink, will be linked to ${PREV_LIBDIR}"
#    entry=$(basename $symlink)
#    ln -s "${CURR_LIBDIR}/${entry}" "${PREV_LIBDIR}/${entry}"
#done
echo "/usr/local/qt5/lib" > /etc/ld.so.conf.d/qt.conf
# Danger!
#sudo dpkg -r --force-depends $(dpkg -l |grep libqt5 | awk '{print $2}')

# Temporary workaround
mkdir -p ${PREV_LIBDIR}/sysqt
for compo in $(find ${PREV_LIBDIR} -name "libQt5*.so.5.9.5" -type f); do
    mv -f ${compo} ${PREV_LIBDIR}/sysqt
done
rm -rf ${PREV_LIBDIR}/libQt5*.so.5
rm -rf ${PREV_LIBDIR}/libQt5*.so.5.9

ldconfig
