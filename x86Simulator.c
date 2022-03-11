/*
 * Author: Daniel Kopta
 * Updated by: Erin Parker
 * CS 4400, University of Utah
 *
 * Simulator handout
 * A simple x86-like processor simulator.
 * Read in a binary file that encodes instructions to execute.
 * Simulate a processor by executing instructions one at a time and appropriately 
 * updating register and memory contents.
 *
 * Some code and pseudo code has been provided as a starting point.
 *
 * Completed by: Brighton Fox
*/

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "instruction.h"

// Forward declarations for helper functions
unsigned int get_file_size(int file_descriptor);
unsigned int* load_file(int file_descriptor, unsigned int size);
instruction_t* decode_instructions(unsigned int* bytes, unsigned int num_instructions);
unsigned int execute_instruction(unsigned int program_counter, instruction_t* instructions, 
				 int* registers, unsigned char* memory);
int setConditionCodes(int reg1, int reg2);
void print_instructions(instruction_t* instructions, unsigned int num_instructions);
void error_exit(const char* message);

// 17 registers
#define NUM_REGS 17
// 1024-byte stack
#define STACK_SIZE 1024

// macros for easy register legibility
#define REG1 registers[instr.first_register]
#define REG2 registers[instr.second_register]
#define IMM instr.immediate
#define ESP registers[6]
#define EFLAGS registers[16]

int main(int argc, char** argv)
{
  // Make sure we have enough arguments
  if(argc < 2)
    error_exit("must provide an argument specifying a binary file to execute");

  // Open the binary file
  int file_descriptor = open(argv[1], O_RDONLY);
  if (file_descriptor == -1) 
    error_exit("unable to open input file");

  // Get the size of the file
  unsigned int file_size = get_file_size(file_descriptor);
  // Make sure the file size is a multiple of 4 bytes
  // since machine code instructions are 4 bytes each
  if(file_size % 4 != 0)
    error_exit("invalid input file");

  // Load the file into memory
  // We use an unsigned int array to represent the raw bytes
  // We could use any 4-byte integer type
  unsigned int* instruction_bytes = load_file(file_descriptor, file_size);
  close(file_descriptor);

  unsigned int num_instructions = file_size / 4;

  // Allocate and decode instructions
  instruction_t* instructions = decode_instructions(instruction_bytes, num_instructions);

  // Optionally print the decoded instructions for debugging
  // print_instructions(instructions, num_instructions);

  // used for reg and mem initializing loops
  int i;
  
  // Allocate and initialize registers to 0 except %esp (index 6), initialize to size of stack in bytes
  int* registers = (int*)malloc(sizeof(int) * NUM_REGS);
  for (i = 0; i < NUM_REGS; i++)
    {
      registers[i] = 0;
    }
  ESP = STACK_SIZE;

  // Stack memory is byte-addressed, so it must be a 1-byte type
  unsigned char* memory = (unsigned char*)malloc(STACK_SIZE*sizeof(unsigned char));
  for (i = 0; i < STACK_SIZE; i++)
    {
      memory[i] = 0;
    }

  // Run the simulation
  unsigned int program_counter = 0;

  // program_counter is a byte address, so we must multiply num_instructions by 4 
  // to get the address past the last instruction
  while(program_counter != num_instructions * 4)
  {
    program_counter = execute_instruction(program_counter, instructions, registers, memory);
  }
  
  
  return 0;
}

/*
 * Decodes the array of raw instruction bytes into an array of instruction_t
 * Each raw instruction is encoded as a 4-byte unsigned int
*/
instruction_t* decode_instructions(unsigned int* bytes, unsigned int num_instructions)
{
  instruction_t* instructions = (instruction_t*)malloc(num_instructions*sizeof(instruction_t));
  
  for(int i = 0; i < num_instructions; i++)
  {
    instructions[i].opcode = bytes[i] >> 27; //bits 27-31
    instructions[i].first_register = (bytes[i] >> 22) & 0x1f; //bits 22-26
    instructions[i].second_register = (bytes[i] >> 17) & 0x1f; //bits 17-21
    instructions[i].immediate = bytes[i] & 0xffff; //bits 22-26    
  }
    
  return instructions;
}


/*
 * Executes a single instruction and returns the next program counter
*/
unsigned int execute_instruction(unsigned int program_counter, instruction_t* instructions, int* registers, unsigned char* memory)
{
  // program_counter is a byte address, but instructions are 4 bytes each
  // divide by 4 to get the index into the instructions array
  instruction_t instr = instructions[program_counter / 4];
  
  switch(instr.opcode)
  {
  case subl:
    REG1 = REG1 - IMM;
    break;
  case addl_reg_reg:
    REG2 = REG1 + REG2;
    break;
  case addl_imm_reg:
    REG1 = REG1 + IMM;
    break;
  case imull:
    REG2 = REG1 * REG2;
    break;
  case shrl:
    REG1 = (unsigned int)REG1 >> 1;
    break;
  case movl_reg_reg:
    REG2 = REG1;
    break;
  case movl_deref_reg:
    REG2 = *(int*)(&memory[REG1 + IMM]);
    break;
  case movl_reg_deref:
    *(int*)(&memory[REG2 + IMM]) = REG1;
    break;
  case movl_imm_reg:
    REG1 = IMM;
    break;
  case cmpl:
    EFLAGS = setConditionCodes(REG1, REG2);
    break;
  case je:
    if ((EFLAGS & 0x00000040) != 0) // ZF
      return program_counter + 4 + IMM;
    break;
  case jl:
  if(((EFLAGS & 0x00000800) != 0) ^ ((EFLAGS & 0x00000080) !=0)) // SF xor OF
      return program_counter + 4 + IMM;
    break;
  case jle:
    if((((EFLAGS & 0x00000800) != 0) ^ ((EFLAGS & 0x00000080) !=0)) || ((EFLAGS & 0x00000040) != 0)) // (SF xor OF) or ZF
      return program_counter + 4 + IMM;
    break;
  case jge:
    if(!(((EFLAGS & 0x00000800) != 0) ^ ((EFLAGS & 0x00000080) !=0))) // not (SF xor OF)
      return program_counter + 4 + IMM;
    break;
  case jbe:
    if(((EFLAGS & 0x00000040) != 0) || ((EFLAGS & 0x00000001) !=0)) // CF or ZF
       return program_counter + 4 + IMM;
    break;
  case jmp:
    return program_counter + 4 + IMM;
    break;
  case call:
    ESP = ESP - 4;
    *(int*)&memory[ESP] = program_counter;
    return program_counter + 4 + IMM;
    break;
  case ret:
    if (ESP == 1024)
      exit(0);
    program_counter = *(int*)&memory[ESP];
    ESP = ESP + 4;
    break;
  case pushl:
    ESP = ESP - 4;
    *(int*)&memory[ESP] = REG1;
    break;
  case popl:
    REG1 = *(int*)&memory[ESP];
    ESP = ESP + 4;
    break;
  case printr:
    printf("%d (0x%x)\n", REG1, REG1);
    break;
  case readr:
    scanf("%d", &(REG1));
    break;
  }

  // TODO: Do not always return program_counter + 4
  //       Some instructions jump elsewhere

  // program_counter + 4 represents the subsequent instruction
  return program_counter + 4;
}

/*
 * Sets CF, ZF, SF, and OF based on passed difference ints
 */
int setConditionCodes(int reg1, int reg2)
{
  int CF = 0;
  int ZF = 0;
  int SF = 0;
  int OF = 0;

  if ((reg1 > 0 && reg2 < -2147483648 + reg1) || (reg1 < 0 && reg2 > 2147483647 + reg1))
  {
    OF = 1;
    if (reg1 < reg2)
    SF = 1;   
  }
  else
  {
    if (reg1 > reg2)
    SF = 1;
  }

  if (((unsigned int)reg2 - (unsigned int)reg1) > (unsigned int)reg2)
    CF = 1;

  if (reg1 == reg2)
    ZF = 1;
  
  return (CF)+
    (ZF << 6)+
    (SF << 7)+
    (OF << 11);
}


/*********************************************/
/****  DO NOT MODIFY THE FUNCTIONS BELOW  ****/
/*********************************************/

/*
 * Returns the file size in bytes of the file referred to by the given descriptor
*/
unsigned int get_file_size(int file_descriptor)
{
  struct stat file_stat;
  fstat(file_descriptor, &file_stat);
  return file_stat.st_size;
}

/*
 * Loads the raw bytes of a file into an array of 4-byte units
*/
unsigned int* load_file(int file_descriptor, unsigned int size)
{
  unsigned int* raw_instruction_bytes = (unsigned int*)malloc(size);
  if(raw_instruction_bytes == NULL)
    error_exit("unable to allocate memory for instruction bytes (something went really wrong)");

  int num_read = read(file_descriptor, raw_instruction_bytes, size);

  if(num_read != size)
    error_exit("unable to read file (something went really wrong)");

  return raw_instruction_bytes;
}

/*
 * Prints the opcode, register IDs, and immediate of every instruction, 
 * assuming they have been decoded into the instructions array
*/
void print_instructions(instruction_t* instructions, unsigned int num_instructions)
{
  printf("instructions: \n");
  unsigned int i;
  for(i = 0; i < num_instructions; i++)
  {
    printf("op: %d, reg1: %d, reg2: %d, imm: %d\n", 
	   instructions[i].opcode,
	   instructions[i].first_register,
	   instructions[i].second_register,
	   instructions[i].immediate);
  }
  printf("--------------\n");
}

/*
 * Prints an error and then exits the program with status 1
*/
void error_exit(const char* message)
{
  printf("Error: %s\n", message);
  exit(1);
}