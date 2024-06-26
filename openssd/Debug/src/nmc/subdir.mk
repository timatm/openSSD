################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../src/nmc/address_translation_nmc.c \
../src/nmc/nmc_file_table.c \
../src/nmc/nmc_mapping.c \
../src/nmc/nmc_requests.c 

OBJS += \
./src/nmc/address_translation_nmc.o \
./src/nmc/nmc_file_table.o \
./src/nmc/nmc_mapping.o \
./src/nmc/nmc_requests.o 

C_DEPS += \
./src/nmc/address_translation_nmc.d \
./src/nmc/nmc_file_table.d \
./src/nmc/nmc_mapping.d \
./src/nmc/nmc_requests.d 


# Each subdirectory must supply rules for building sources it contributes
src/nmc/%.o: ../src/nmc/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: ARM v7 gcc compiler'
	arm-none-eabi-gcc -DDEBUG -Wall -O0 -g3 -I"Z:\vm\openSSD-fw-ori\openssd\src" -I"Z:\vm\openSSD-fw-ori\openssd\src\nvme" -c -fmessage-length=0 -MT"$@" -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard -I../../openssd_bsp/ps7_cortexa9_0/include -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


