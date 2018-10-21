/*
 * This handles any SCSI OP 'mode sense / mode select'
 *
 * Copyright (C) 2005 - 2009 Mark Harvey markh794 at gmail dot com
 *                                mark.harvey at nutanix dot com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * See comments in vtltape.c for a more complete version release...
 *
 */

int add_mode_page_rw_err_recovery(struct lu_phy_attr *lu);
int add_mode_disconnect_reconnect(struct lu_phy_attr *lu);
int add_mode_control(struct lu_phy_attr *lu);
int add_mode_control_extension(struct lu_phy_attr *lu);
int add_mode_data_compression(struct lu_phy_attr *lu);
int add_mode_device_configuration(struct lu_phy_attr *lu);
int add_mode_device_configuration_extention(struct lu_phy_attr *lu);
int add_mode_medium_partition(struct lu_phy_attr *lu);
int add_mode_power_condition(struct lu_phy_attr *lu);
int add_mode_information_exception(struct lu_phy_attr *lu);
int add_mode_medium_configuration(struct lu_phy_attr *lu);
int add_mode_ait_device_configuration(struct lu_phy_attr *lu);
int add_mode_ult_encr_mode_pages(struct lu_phy_attr *lu);
int add_mode_vendor_25h_mode_pages(struct lu_phy_attr *lu);
int add_mode_encryption_mode_attribute(struct lu_phy_attr *lu);
int add_mode_behavior_configuration(struct lu_phy_attr *lu);

int add_mode_device_capabilities(struct lu_phy_attr *lu);
int add_mode_transport_geometry(struct lu_phy_attr *lu);
int add_mode_element_address_assignment(struct lu_phy_attr *lu);
int update_prog_early_warning(struct lu_phy_attr *lu);
void dealloc_all_mode_pages(struct lu_phy_attr *lu);
int add_smc_mode_page_drive_configuration(struct lu_phy_attr *lu);
