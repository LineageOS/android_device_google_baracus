#
# Copyright (C) The LineageOS Project
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

-include device/nvidia/foster/AndroidBoard.mk

# Path setup
INSTALLED_DTB_SRC := $(PRODUCT_OUT)/install/tegra210-baracus.dtb
INSTALLED_DTB_IMG := $(PRODUCT_OUT)/install/dtb.img

# Add a phony target to copy DTB -> dtb.img
$(INSTALLED_DTB_IMG): $(INSTALLED_DTB_SRC)
	@echo "Copying DTB to dtb.img"
	$(hide) cp -f $< $@

# Ensure dtb.img is included in the build
ALL_DEFAULT_INSTALLED_MODULES += $(INSTALLED_DTB_IMG)

INSTALLED_RADIOIMAGE_TARGET += $(INSTALLED_DTB_IMG)
