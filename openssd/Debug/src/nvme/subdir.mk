################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../src/nvme/host_lld.c \
../src/nvme/nvme_admin_cmd.c \
../src/nvme/nvme_identify.c \
../src/nvme/nvme_io_cmd.c \
../src/nvme/nvme_main.c 

OBJS += \
./src/nvme/host_lld.o \
./src/nvme/nvme_admin_cmd.o \
./src/nvme/nvme_identify.o \
./src/nvme/nvme_io_cmd.o \
./src/nvme/nvme_main.o 

C_DEPS += \
./src/nvme/host_lld.d \
./src/nvme/nvme_admin_cmd.d \
./src/nvme/nvme_identify.d \
./src/nvme/nvme_io_cmd.d \
./src/nvme/nvme_main.d 


# Each subdirectory must supply rules for building sources it contributes
src/nvme/%.o: ../src/nvme/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: ARM v7 gcc compiler'
	arm-none-eabi-gcc -DDEBUG -Wall -O0 -g3 -I"Z:\vm\openSSD-fw-ori\openssd\src" -I"Z:\vm\openSSD-fw-ori\openssd\src\nvme" -c -fmessage-length=0 -MT"$@" -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard -I../../openssd_bsp/ps7_cortexa9_0/include -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


