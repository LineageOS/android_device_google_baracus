#
# Copyright (C) 2024 The LineageOS Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Bootloader
TARGET_TEGRA_UBOOT_CONFIG := baracus_defconfig

# Fingerprint
PRODUCT_BUILD_PROP_OVERRIDES += \
    BuildDesc="baracus-user 7.0 NRT1.240624.004 12020658 release-keys" \
    BuildFingerprint=google/baracus/baracus:7.0/NRT1.240624.004/12020658:user/release-keys \
    DeviceProduct=baracus \
    SystemName=baracus

# Touch
PRODUCT_PACKAGES += input-port-associations.xml

# Scaler UART daemon
PRODUCT_PACKAGES += scalerd

# Input source switcher (Settings + QS tile)
PRODUCT_PACKAGES += JamboardInputSource

# Touch forwarding HID gadget
PRODUCT_PACKAGES += \
    touch_forward \
    touch_enable \
    hid_mt_report_desc

PRODUCT_COPY_FILES += \
    device/nvidia/foster/initfiles/init.baracus.touch.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.baracus.touch.rc

$(call inherit-product, device/nvidia/foster/device.mk)
