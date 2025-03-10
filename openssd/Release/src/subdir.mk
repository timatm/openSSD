################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
LD_SRCS += \
../src/lscript.ld 

C_SRCS += \
../src/address_translation.c \
../src/data_buffer.c \
../src/ftl_config.c \
../src/garbage_collection.c \
../src/main.c \
../src/nsc_driver.c \
../src/request_allocation.c \
../src/request_schedule.c \
../src/request_transform.c 

OBJS += \
./src/address_translation.o \
./src/data_buffer.o \
./src/ftl_config.o \
./src/garbage_collection.o \
./src/main.o \
./src/nsc_driver.o \
./src/request_allocation.o \
./src/request_schedule.o \
./src/request_transform.o 

C_DEPS += \
./src/address_translation.d \
./src/data_buffer.d \
./src/ftl_config.d \
./src/garbage_collection.d \
./src/main.d \
./src/nsc_driver.d \
./src/request_allocation.d \
./src/request_schedule.d \
./src/request_transform.d 


# Each subdirectory must supply rules for building sources it contributes
src/%.o: ../src/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: ARM v7 gcc compiler'
	arm-none-eabi-gcc -Wall -O2 -I"Z:\vm\openSSD-fw-ori\openssd\src" -I"Z:\vm\openSSD-fw-ori\openssd\src\nvme" -c -fmessage-length=0 -MT"$@" -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard -I../../openssd_bsp/ps7_cortexa9_0/include -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


