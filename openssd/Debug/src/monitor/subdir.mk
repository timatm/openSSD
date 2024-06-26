################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../src/monitor/monitor.c \
../src/monitor/monitor_buffer.c \
../src/monitor/monitor_cmds.c \
../src/monitor/monitor_flash.c \
../src/monitor/monitor_map.c 

OBJS += \
./src/monitor/monitor.o \
./src/monitor/monitor_buffer.o \
./src/monitor/monitor_cmds.o \
./src/monitor/monitor_flash.o \
./src/monitor/monitor_map.o 

C_DEPS += \
./src/monitor/monitor.d \
./src/monitor/monitor_buffer.d \
./src/monitor/monitor_cmds.d \
./src/monitor/monitor_flash.d \
./src/monitor/monitor_map.d 


# Each subdirectory must supply rules for building sources it contributes
src/monitor/%.o: ../src/monitor/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: ARM v7 gcc compiler'
	arm-none-eabi-gcc -DDEBUG -Wall -O0 -g3 -I"Z:\vm\openSSD-fw-ori\openssd\src" -I"Z:\vm\openSSD-fw-ori\openssd\src\nvme" -c -fmessage-length=0 -MT"$@" -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard -I../../openssd_bsp/ps7_cortexa9_0/include -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


