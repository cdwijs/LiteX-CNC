# Welcome to LiteX-CNC!

This project aims to make a generic CNC firmware and driver for FPGA cards which are supported by LiteX. Configuration of the board and driver is done using json-files. The supported boards are the Colorlight boards 5A-75B and 5A-75E, as these are fully supported with the open source toolchain.

> **RV901T** <br>
> Although the RV901T is also supported by Litex, the firmware cannot be automatically build with LitexCNC, as it requires the Xilinx-software to compile the Verilog to a bit-stream. LitexCNC can be used to create the Verilog and the driver will work when the bit-stream is loaded on the board. However, there is a gap in the toolchain not covered. There are known issues with the compantibility of Litex with Xilinx.

The idea of this project was conceived by ColorCNC by *romanetz* on the official LinuxCNC and the difficulty to obtain a MESA card.

> **Experimental** <br>
> At this moment this code is experimental and requires expansion. A test card has been received and a test setup has been created with an Raspberry Pi. The modules GPIO, PWM, Stepgen are tested and are working. Expansion of the project with encoders, I2C, RS489, etc. is now required.

## Acknowledgements
This project would not be possible without:
- ColorCNC by *romanetz* [link](https://forum.linuxcnc.org/27-driver-boards/44422-colorcnc?start=0);
- HostMot2 (MESA card driver) as the structure of the driver has been adopted.

## Installation
LitexCNC can be installed using pip:
```shell
pip install litexcnc[cli]
```

After installation of LitexCNC, one can setup building environment for the firmware using the included scripts:
```shell
litexcnc install_litex
litexcnc install_toolchain
```

Both Litex and the toolchain (OSS-CAD-suite) will be installed by default be installed in the `/opt` folder. Optionally the flag `--user` can be supplied to both commands, in which case the building environment is installed in `HOME`-directory.

## Configuration of the FPGA

### Structure of the JSON file
...

### Building the firmware (bit-file)
The firmare can be created based with the following command:
```shell
litexcnc build_firmware "<path-to-your-configuration>" --build 
```

### Compiling the driver

> Compilation of the driver is only required once as long the same version of LitexCNC is used. When LitexCNC is updated, please re-install the driver; the version of the firmware should always be the same as the version of the driver. 

The firmare can be created based with the following command:
```shell
litexcnc install_driver
```

This script will run `apt-get` to install the following packages:
- `libjson-c-dev`, which is required to read the configuration files. 
- `linuxcnc-dev`, which is required for running `halcompile`.



After this, the driver is installed using `halcompile`.

## Usage in HAL
Typically main litexcnc driver is loaded first:
```
loadrt litexcnc
```

After loading the main driver, the board-driver can be loaded. At this moment only ethernet cards are supported using the `litexcnc_eth` board-driver. All the board-driver modules accept a load-time modparam of type string array, named `config_file`. This array has one config_file string for each board the driver should use. Each json-file is passed to and parsed by the litexcnc driver when the board-driver registers the board. The paths can contain spaces, so it is usually a good idea to wrap the whole thing in double-quotes (the " character). The comma character (,) separates members of the config array from each other.
```
loadrt litexcnc_eth config_file="/workspace/examples/5a-75e.json"
```

### Functions

The table below gives the functions exported by LiteX-CNC. 

| Function        | Description |
|-----------------|-------------|
| litexcnc_<BoardName>.<BoardNum>.read | This reads the encoder counters, stepgen feedbacks, and GPIO input pins from the FPGA. |
| litexcnc_<BoardName>.<BoardNum>.write     | This updates the PWM duty cycles, stepgen rates, and GPIO outputs on the FPGA. Any changes to configuration pins such as stepgen timing, GPIO inversions, etc, are also effected by this function. |

Example:
```
loadrt threads name1=servo-thread period1=1000000
addf test.0.read servo-thread
# Add any function between the `read` and `write` routine of LitexCNC
addf test.0.write servo-thread
```

### Pins and parameters

#### GPIO

> By default, the name of the pin is as defined in the JSON and uses the LiteX name of the pin (for example `j13:6`, meaning connector 13, pin 6). When in the in the josn-configutation the field `alias` is set, this name will be used instead.

| Pin        | Type | Description |
|------------|------|-------------|
|litexcnc_<BoardName>.<BoardNum>.<PinName>.in | (bit out) | State of the hardware input pin.
|litexcnc_<BoardName>.<BoardNum>.<PinName>.in_not | (bit out) | Inverted state of the hardware input pin.
|litexcnc_<BoardName>.<BoardNum>.<PinName>.out | (bit in) | Value to be written (possibly inverted) to the hardware output pin.

| Parameter  | Type | Description |
|------------|------|-------------|
|litexcnc_<BoardName>.<BoardNum>.<PinName>.invert_output | (bit in) | If this parameter is true, the output value of the GPIO will be the inverse of the value on the "out" HAL pin.

## Supported boards
All boards which support [LiteEth](https://github.com/enjoy-digital/liteeth) are supported, this includes the re-purposed Colorlight 5a-75a, 5a-75e, i5 and i9 boards. A full list of boards supported by LiteX and whether LiteEth is supported can be found [here](https://github.com/litex-hub/litex-boards).

The structure of the driver allows to add more communication protocols in the future. I'm looking forward to add USB support as well. However, due to the nature of USB-communication this will most likely not result in real-time behaviour.
