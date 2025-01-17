//
//    Copyright (C) 2022 Peter van Tol
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; either version 2 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program; if not, write to the Free Software
//    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
//
#include <stdio.h>
#include <json-c/json.h>
#include <time.h>

#include <rtapi_slab.h>
#include <rtapi_ctype.h>
#include <rtapi_list.h>

#include "rtapi.h"
#include "rtapi_app.h"
#include "rtapi_string.h"
#include "rtapi_math.h"

#include "hal.h"

#include "litexcnc.h"
#include "gpio.h"
#include "crc.h"


#ifdef MODULE_INFO
MODULE_INFO(linuxcnc, "component:litexcnc:Board driver for FPGA boards supported by litex.");
MODULE_INFO(linuxcnc, "funct:read:1:Read all registers.");
MODULE_INFO(linuxcnc, "funct:write:1:Write all registers, and pet the watchdog to keep it from biting.");
MODULE_INFO(linuxcnc, "author:Peter van Tol petertgvantolATgmailDOTcom");
MODULE_INFO(linuxcnc, "license:GPL");
MODULE_LICENSE("GPL");
#endif // MODULE_INFO


// This keeps track of all the litexcnc instances that have been registered by drivers
struct rtapi_list_head litexcnc_list;

// This keeps track of the component id. Required for setup and tear down.
static int comp_id;

static void litexcnc_config(void* void_litexcnc, long period) {
    litexcnc_t *litexcnc = void_litexcnc;

    // Clear buffer
    uint8_t *config_buffer = rtapi_kmalloc(LITEXCNC_CONFIG_HEADER_SIZE, RTAPI_GFP_KERNEL);
    memset(config_buffer, 0, LITEXCNC_CONFIG_HEADER_SIZE);
    
    // Configure all the functions
    uint8_t* pointer = config_buffer;

    // Configure the general settings
    litexcnc_config_header_t config_data;
    config_data.loop_cycles = htobe32((int32_t)((double) litexcnc->clock_frequency * period * 0.000000001));
    memcpy(pointer, &config_data, sizeof(litexcnc_config_header_t));
    pointer += sizeof(litexcnc_config_header_t);

    // Configure all the functions
    // litexcnc_watchdog_config(litexcnc, &pointer, period);
    // litexcnc_wallclock_config(litexcnc, &pointer, period);
    // litexcnc_gpio_config(litexcnc, &pointer, period);
    // litexcnc_pwm_config(litexcnc, &pointer, period);
    litexcnc_stepgen_config(litexcnc, &pointer, period);
    // litexcnc_encoder_config(litexcnc, &pointer, period);
    
    // Write the data to the FPGA
    litexcnc->fpga->write_config(litexcnc->fpga, config_buffer, LITEXCNC_CONFIG_HEADER_SIZE);
}


static void litexcnc_read(void* void_litexcnc, long period) {
    litexcnc_t *litexcnc = void_litexcnc;

    // The first loop no data is read, as it is used for sending the configuration to the 
    // FPGA. The configuration is written in the `litexcnc_write` function. 
    if (!litexcnc->read_loop_has_run) {
        litexcnc->read_loop_has_run = true;
        return;
    }

    // Clear buffer
    memset(litexcnc->fpga->read_buffer, 0, litexcnc->fpga->read_buffer_size);
    
    // Read the state from the FPGA
    litexcnc->fpga->read(litexcnc->fpga);

    // TODO: don't process the read data in case the read has failed.

    // Process the read data for the different compenents
    uint8_t* pointer = litexcnc->fpga->read_buffer;
    litexcnc_watchdog_process_read(litexcnc, &pointer);
    litexcnc_wallclock_process_read(litexcnc, &pointer);
    litexcnc_gpio_process_read(litexcnc, &pointer);
    litexcnc_pwm_process_read(litexcnc, &pointer);
    litexcnc_stepgen_process_read(litexcnc, &pointer, period);
    litexcnc_encoder_process_read(litexcnc, &pointer, period);
}

static void litexcnc_write(void *void_litexcnc, long period) {
    litexcnc_t *litexcnc = void_litexcnc;

    // Check whether the write has been initialized AND the read and write functions
    // are in the recommended order (first read, then write). In the first loop the
    // we don't write any data to the FPGA, but it is configured. This is required,
    // because the configuration requires the period to be known, which prevents the
    // configuration to be performed before the HAL-loop starts
    if (!litexcnc->write_loop_has_run) {
        // Check whether the read cycle has been run, if not, the order is not correct
        if (!litexcnc->read_loop_has_run) {
            LITEXCNC_WARN("Read and write functions in incorrect order. Recommended order is read first, then write.\n", litexcnc->fpga->name);
        }
        // Configure the FPGA and set flag that the write function has been done once
        litexcnc_config(void_litexcnc, period);
        litexcnc->write_loop_has_run = true;
        return;
    }

    // Clear buffer
    memset(litexcnc->fpga->write_buffer, 0, litexcnc->fpga->write_buffer_size);

    // Process all functions
    uint8_t* pointer = litexcnc->fpga->write_buffer;
    litexcnc_watchdog_prepare_write(litexcnc, &pointer, period);
    litexcnc_wallclock_prepare_write(litexcnc, &pointer);
    litexcnc_gpio_prepare_write(litexcnc, &pointer);
    litexcnc_pwm_prepare_write(litexcnc, &pointer);
    litexcnc_stepgen_prepare_write(litexcnc, &pointer, period);
    litexcnc_encoder_prepare_write(litexcnc, &pointer, period);

    // Write the data to the FPGA
    litexcnc->fpga->write(litexcnc->fpga);
}


static void litexcnc_cleanup(litexcnc_t *litexcnc) {
    // clean up the Pins, if they're initialized
    // if (litexcnc->pin != NULL) rtapi_kfree(litexcnc->pin);

    // clean up the Modules
    // TODO
}


EXPORT_SYMBOL_GPL(litexcnc_register);
int litexcnc_register(litexcnc_fpga_t *fpga, const char *config_file) {
    int r;
    litexcnc_t *litexcnc;

    // Allocate memory for the board
    litexcnc = rtapi_kmalloc(sizeof(litexcnc_t), RTAPI_GFP_KERNEL);
    if (litexcnc == NULL) {
        LITEXCNC_PRINT_NO_DEVICE("out of memory!\n");
        return -ENOMEM;
    }
    memset(litexcnc, 0, sizeof(litexcnc_t));

    // Set initial values
    litexcnc->write_loop_has_run = false;
    litexcnc->read_loop_has_run = false;

    // Store the FPGA on it
    litexcnc->fpga = fpga;
    
    // Add it to the list
    rtapi_list_add_tail(&litexcnc->list, &litexcnc_list);

    // Calculate the CRC-fingerprint of the board
    // - init properties
    FILE *fileptr;
    unsigned char *buffer;
    long filelen;
    // - open file get the length of the file
    fileptr = fopen(config_file, "rb");  // Open the file in binary mode
    fseek(fileptr, 0, SEEK_END);          // Jump to the end of the file
    filelen = ftell(fileptr);             // Get the current byte offset in the file
    rewind(fileptr);                      // Jump back to the beginning of the file
    // - create buffer, read contents and close file
    buffer = (unsigned char *)malloc(filelen * sizeof(unsigned char)); // Enough memory for the file
    fread(buffer, filelen, 1, fileptr);   // Read in the entire file
    fclose(fileptr);                      // Close the file
    // - calculate the CRC-value and store it on the config
    litexcnc->config_fingerprint = crc32(buffer, filelen, 0);

    // Verify the fingerprint of the FPGA
    r = litexcnc->fpga->verify_config(litexcnc->fpga);
    if (r == 0) {
        // Check version of firmware of driver and firmware
        if (((litexcnc->fpga->version >> 16) & 0xff) != LITEXCNC_VERSION_MAJOR || ((litexcnc->fpga->version >> 8) & 0xff) != LITEXCNC_VERSION_MINOR )  {
            // Incompatible version
            LITEXCNC_ERR_NO_DEVICE(
                "Version of firmware (%u.%u.%u) is incompatible with the version of the driver (%u.%u.%u) \n",
                (litexcnc->fpga->version >> 16) & 0xff, (litexcnc->fpga->version >> 8) & 0xff, (litexcnc->fpga->version) & 0xff, 
                LITEXCNC_VERSION_MAJOR, LITEXCNC_VERSION_MINOR, LITEXCNC_VERSION_PATCH);
            r = -1;
        } else if ((litexcnc->fpga->version & 0xff) != LITEXCNC_VERSION_PATCH) {
            // Warn that patch version is different
            LITEXCNC_PRINT_NO_DEVICE(
                "INFO: Version of firmware (%u.%u.%u) is different with the version of the driver (%u.%u.%u). Communication is still possible, although one of these could use an update for the best experience. \n",
                (litexcnc->fpga->version >> 16) & 0xff, (litexcnc->fpga->version >> 8) & 0xff, (litexcnc->fpga->version) & 0xff, 
                LITEXCNC_VERSION_MAJOR, LITEXCNC_VERSION_MINOR, LITEXCNC_VERSION_PATCH);
        }
        // Check fingerprint
        if (litexcnc->config_fingerprint != litexcnc->fpga->fingerprint) {
            LITEXCNC_ERR_NO_DEVICE(
                "Fingerprint incorrect (driver: %08d, FPGA: %08d)\n", 
                litexcnc->config_fingerprint, 
                litexcnc->fpga->fingerprint);
            r = -1;
        }
    }
    if (r != 0) {
        LITEXCNC_ERR_NO_DEVICE("Validation of config failed.\n");
        goto fail0;
    }

    // Initialize the functions
    struct json_object *config = json_object_from_file(config_file);
    if (!config) {
        LITEXCNC_ERR_NO_DEVICE("Could not load configuration file '%s'\n", config_file);
        goto fail0;
    }

    // Store the name of the board
    struct json_object *board_name;
    if (!json_object_object_get_ex(config, "board_name", &board_name)) {
        LITEXCNC_WARN_NO_DEVICE("Missing optional JSON key: '%s'\n", "board_name");
        json_object_put(board_name);
        rtapi_snprintf(fpga->name, sizeof(fpga->name), "litexcnc.0"); //TODO: add counter
    } else {
        rtapi_snprintf(fpga->name, sizeof(fpga->name), json_object_get_string(board_name));
    }
    // Check the name of the board for validity
    size_t i;
    for (i = 0; i < HAL_NAME_LEN+1; i ++) {
        if (fpga->name[i] == '\0') break;
        if (!isprint(fpga->name[i])) {
            LITEXCNC_ERR_NO_DEVICE("Invalid board name (contains non-printable character)\n");
            r = -EINVAL;
            goto fail1;
        }
    }
    if (i == HAL_NAME_LEN+1) {
        LITEXCNC_ERR_NO_DEVICE("Invalid board name (not NULL terminated)\n");
        r = -EINVAL;
        goto fail1;
    }
    if (i == 0) {
        LITEXCNC_ERR_NO_DEVICE("Invalid board name (zero length)\n");
        r = -EINVAL;
        goto fail1;
    }    

    // Store the clock-frequency from the file
    struct json_object *clock_frequency;
    if (!json_object_object_get_ex(config, "clock_frequency", &clock_frequency)) {
        LITEXCNC_ERR_NO_DEVICE("Missing required JSON key: '%s'\n", "clock_frequency");
        json_object_put(clock_frequency);
        json_object_put(config);
        goto fail1;
    }
    litexcnc->clock_frequency = json_object_get_int(clock_frequency);
    litexcnc->clock_frequency_recip = 1.0f / litexcnc->clock_frequency;
    json_object_put(clock_frequency);
    // Initialize modules
    LITEXCNC_PRINT_NO_DEVICE("Setting up modules...\n");
    LITEXCNC_PRINT_NO_DEVICE(" - Watchdog\n");
    if (litexcnc_watchdog_init(litexcnc, config) < 0) {
        LITEXCNC_ERR_NO_DEVICE("Watchdog init failed\n");
        goto fail0;
    }
    LITEXCNC_PRINT_NO_DEVICE(" - Wallclock\n");
    if (litexcnc_wallclock_init(litexcnc, config) < 0) {
        LITEXCNC_ERR_NO_DEVICE("Wallclock init failed\n");
        goto fail0;
    }
    LITEXCNC_PRINT_NO_DEVICE(" - GPIO\n");
    if (litexcnc_gpio_init(litexcnc, config) < 0) {
        LITEXCNC_ERR_NO_DEVICE("GPIO init failed\n");
        goto fail0;
    }
    LITEXCNC_PRINT_NO_DEVICE(" - PWM\n");
    if (litexcnc_pwm_init(litexcnc, config) < 0) {
        LITEXCNC_ERR_NO_DEVICE("PWM init failed\n");
        goto fail0;
    }
    LITEXCNC_PRINT_NO_DEVICE(" - Stepgen\n");
    if (litexcnc_stepgen_init(litexcnc, config) < 0) {
        LITEXCNC_ERR_NO_DEVICE("Stepgen init failed\n");
        goto fail0;
    }
    LITEXCNC_PRINT_NO_DEVICE(" - Encoder\n");
    if (litexcnc_encoder_init(litexcnc, config) < 0) {
        LITEXCNC_ERR_NO_DEVICE("Encoder init failed\n");
        goto fail0;
    }


    // Create the buffers for reading and writing data
    LITEXCNC_PRINT_NO_DEVICE("Creating read and write buffers...\n");
    // - write buffer
    litexcnc->fpga->write_buffer_size = LITEXCNC_BOARD_DATA_WRITE_SIZE(litexcnc);
    LITEXCNC_PRINT_NO_DEVICE(" - Write buffer: %d bytes)\n", LITEXCNC_BOARD_DATA_WRITE_SIZE(litexcnc));
    uint8_t *write_buffer = rtapi_kmalloc(litexcnc->fpga->write_buffer_size, RTAPI_GFP_KERNEL);
    if (litexcnc == NULL) {
        LITEXCNC_PRINT_NO_DEVICE("out of memory!\n");
        r = -ENOMEM;
        goto fail1;
    }
    memset(write_buffer, 0, litexcnc->fpga->write_buffer_size);
    litexcnc->fpga->write_buffer = write_buffer;
    // - read buffer
    LITEXCNC_PRINT_NO_DEVICE(" - Read buffer: %d bytes)\n", LITEXCNC_BOARD_DATA_READ_SIZE(litexcnc));
    litexcnc->fpga->read_buffer_size = LITEXCNC_BOARD_DATA_READ_SIZE(litexcnc);
    uint8_t *read_buffer = rtapi_kmalloc(litexcnc->fpga->read_buffer_size, RTAPI_GFP_KERNEL);
    if (litexcnc == NULL) {
        LITEXCNC_PRINT_NO_DEVICE("out of memory!\n");
        r = -ENOMEM;
        goto fail1;
    }
    memset(read_buffer, 0, litexcnc->fpga->read_buffer_size);
    litexcnc->fpga->read_buffer = read_buffer;

    // Export functions
    LITEXCNC_PRINT_NO_DEVICE("Exporting functions...\n");
    // // - communicate function
    // char name[HAL_NAME_LEN + 1];
    // rtapi_snprintf(name, sizeof(name), "%s.communicate", litexcnc->fpga->name);
    // r = hal_export_funct(name, litexcnc_communicate, litexcnc, 1, 0, litexcnc->fpga->comp_id);
    // if (r != 0) {
    //     LITEXCNC_ERR("Error %d exporting `communicate` function %s\n", r, name);
    //     r = -EINVAL;
    //     goto fail1;
    // }
    // - read function
    char name[HAL_NAME_LEN + 1];
    rtapi_snprintf(name, sizeof(name), "%s.read", litexcnc->fpga->name);
    r = hal_export_funct(name, litexcnc_read, litexcnc, 1, 0, litexcnc->fpga->comp_id);
    if (r != 0) {
        LITEXCNC_ERR("error %d exporting read function %s\n", r, name);
        r = -EINVAL;
        goto fail1;
    }
    // - write function
    rtapi_snprintf(name, sizeof(name), "%s.write", litexcnc->fpga->name);
    r = hal_export_funct(name, litexcnc_write, litexcnc, 1, 0, litexcnc->fpga->comp_id);
    if (r != 0) {
        LITEXCNC_ERR("error %d exporting read function %s\n", r, name);
        r = -EINVAL;
        goto fail1;
    }

    r = litexcnc->fpga->post_register(litexcnc->fpga);
    if (r != 0) {
        LITEXCNC_PRINT_NO_DEVICE("Registration hooks failed \n");
        goto fail1;
    }

    // Reset the FPGA
    r = litexcnc->fpga->reset(litexcnc->fpga);
    if (r != 0) {
        LITEXCNC_PRINT_NO_DEVICE("Reset of FPGA failed \n");
        goto fail1;
    }

    return 0;

fail1:
    litexcnc_cleanup(litexcnc);  // undoes the rtapi_kmallocs from hm2_parse_module_descriptors()

fail0:
    rtapi_list_del(&litexcnc->list);
    rtapi_kfree(litexcnc);
    return r;
}


int rtapi_app_main(void) {
    LITEXCNC_PRINT_NO_DEVICE("Loading Litex CNC driver version %u.%u.%u\n", LITEXCNC_VERSION_MAJOR, LITEXCNC_VERSION_MINOR, LITEXCNC_VERSION_PATCH);

    comp_id = hal_init(LITEXCNC_NAME);
    if(comp_id < 0) return comp_id;
    
    RTAPI_INIT_LIST_HEAD(&litexcnc_list);

    hal_ready(comp_id);

    return 0;
}


void rtapi_app_exit(void) {
    hal_exit(comp_id);
    LITEXCNC_PRINT_NO_DEVICE("LitexCNC driver unloaded \n");
}


// Include all other files. This makes separation in different files possible,
// while still compiling a single file. NOTE: the #include directive just copies
// the whole contents of that file into this source-file.
#include "crc.c"
#include "watchdog.c"
#include "wallclock.c"
#include "gpio.c"
#include "pwm.c"
#include "stepgen.c"
#include "encoder.c"