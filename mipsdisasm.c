#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <capstone/capstone.h>

#include "mipsdisasm.h"
#include "utils.h"

#define MIPSDISASM_VERSION "0.2+"

// typedefs
typedef struct
{
   char name[59];
   char global;
   unsigned int vaddr;
} asm_label;

typedef struct
{
   int linked_insn;
   union
   {
      unsigned int linked_value;
      float linked_float;
   };
   int newline;
} disasm_extra;

// hidden disassembler state struct
typedef struct _disasm_state
{
   asm_label *labels;
   int label_alloc;
   int label_count;

   csh handle;
   cs_insn *instructions;
   disasm_extra *insn_extra;
   int instruction_count;

   unsigned int vaddr;

   asm_syntax syntax;
} disasm_state;

// state: disassemble state to search for known label
// vaddr: virtual address to find
// returns index in state labels if found, -1 otherwise
static int label_find(const disasm_state *state, unsigned int vaddr)
{
   for (int i = 0; i < state->label_count; i++) {
      if (state->labels[i].vaddr == vaddr) {
         return i;
      }
   }
   return -1;
}

// add label to state even if one already exists at that address
// state: disassemble state to add label
// vaddr: virtual address of label
// global: true if global, false if local
static void label_add(disasm_state *state, const char *name, unsigned int vaddr, char global)
{
   if (state->label_count >= state->label_alloc) {
      state->label_alloc *= 2;
      state->labels = realloc(state->labels, sizeof(*state->labels) * state->label_alloc);
   }
   asm_label *l = &state->labels[state->label_count];
   strcpy(l->name, name);
   l->global = global;
   l->vaddr = vaddr;
   state->label_count++;
}

static int cmp_label(const void *a, const void *b)
{
   const asm_label *ala = a;
   const asm_label *alb = b;
   // first sort by vaddr, then by global, then by name
   if (ala->vaddr > alb->vaddr) {
      return 1;
   } else if (alb->vaddr > ala->vaddr) {
      return -1;
   } else {
      if (ala->global > alb->global) {
         return 1;
      } else if (alb->global > ala->global) {
         return -1;
      } else {
         return strcmp(ala->name, alb->name);
      }
   }
}

// try to find a matching LUI for a given register
static void link_with_lui(disasm_state *state, int offset, unsigned int reg, unsigned int mem_imm)
{
#define MAX_LOOKBACK 128
   cs_insn *insn = state->instructions;
   // don't attempt to compute addresses for zero offset
   if (mem_imm != 0x0) {
      // end search after some sane max number of instructions
      int end_search = MAX(0, offset - MAX_LOOKBACK);
      for (int search = offset - 1; search >= end_search; search--) {
         // use an `if` instead of `case` block to allow breaking out of the `for` loop
         if (insn[search].id == MIPS_INS_LUI) {
            unsigned int rd = insn[search].detail->mips.operands[0].reg;
            if (reg == rd) {
               unsigned int lui_imm = (unsigned int)insn[search].detail->mips.operands[1].imm;
               unsigned int addr = ((lui_imm << 16) + mem_imm);
               state->insn_extra[search].linked_insn = offset;
               state->insn_extra[search].linked_value = addr;
               state->insn_extra[offset].linked_insn = search;
               state->insn_extra[offset].linked_value = addr;
               // if not ORI, create global data label if one does not exist
               if (insn[offset].id != MIPS_INS_ORI) {
                  int label = label_find(state, addr);
                  if (label < 0) {
                     char label_name[32];
                     sprintf(label_name, "D_%08X", addr);
                     label_add(state, label_name, addr, 1);
                  }
               }
               break;
            }
         } else if (insn[search].id == MIPS_INS_LW ||
                    insn[search].id == MIPS_INS_LD ||
                    insn[search].id == MIPS_INS_ADDIU ||
                    insn[search].id == MIPS_INS_ADD ||
                    insn[search].id == MIPS_INS_SUB ||
                    insn[search].id == MIPS_INS_SUBU) {
            unsigned int rd = insn[search].detail->mips.operands[0].reg;
            if (reg == rd) {
               // ignore: reg is pointer, offset is probably struct data member
               break;
            }
         } else if (insn[search].id == MIPS_INS_JR &&
               insn[search].detail->mips.operands[0].reg == MIPS_REG_RA) {
            // stop looking when previous `jr ra` is hit
            break;
         }
      }
   }
}

// disassemble a block of code and collect JALs and local labels
static void disassemble_block(unsigned char *data, size_t data_len, unsigned int vaddr, disasm_state *state, int merge_pseudo)
{
   csh handle;
   cs_insn *insn;
   int count;

   // open capstone disassembler
   if (cs_open(CS_ARCH_MIPS, CS_MODE_MIPS64 + CS_MODE_BIG_ENDIAN, &handle) != CS_ERR_OK) {
      ERROR("Error initializing disassembler\n");
      exit(EXIT_FAILURE);
   }
   cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
   cs_option(handle, CS_OPT_SKIPDATA, CS_OPT_ON);

   count = cs_disasm(handle, data, data_len, vaddr, 0, &insn);
   if (count > 0) {
      state->instructions = insn;
      state->instruction_count = count;
      state->handle = handle;
      state->vaddr = vaddr;
      state->insn_extra = calloc(count, sizeof(*state->insn_extra));
      for (int i = 0; i < count; i++) {
         cs_mips *mips = &insn[i].detail->mips;
         state->insn_extra[i].linked_insn = -1;
         if (cs_insn_group(handle, &insn[i], MIPS_GRP_JUMP)) {
            if (insn[i].id == MIPS_INS_JR || insn[i].id == MIPS_INS_JALR) {
               if (insn[i].detail->mips.operands[0].reg == MIPS_REG_RA &&  i + 2 < count) {
                  state->insn_extra[i + 2].newline = 1;
               }
            } else {
               // all branches and jumps
               for (int o = 0; o < mips->op_count; o++) {
                  if (mips->operands[o].type == MIPS_OP_IMM)
                  {
                     char label_name[32];
                     unsigned int branch_target = (unsigned int)mips->operands[o].imm;
                     // create label if one does not exist
                     int label = label_find(state, branch_target);
                     if (label < 0) {
                        switch (state->syntax) {
                           case ASM_GAS:    sprintf(label_name, ".L%08X", branch_target); break;
                           case ASM_ARMIPS: sprintf(label_name, "@L%08X", branch_target); break;
                        }
                        label_add(state, label_name, branch_target, 0);
                     }
                  }
               }
            }
         } else if (insn[i].id == MIPS_INS_JAL || insn[i].id == MIPS_INS_BAL) {
            unsigned int jal_target  = (unsigned int)mips->operands[0].imm;
            // create label if one does not exist
            if (label_find(state, jal_target) < 0) {
               char label_name[32];
               sprintf(label_name, "func_%08X", jal_target);
               label_add(state, label_name, jal_target, 1);
            }
         }

         if (merge_pseudo) {
            switch (insn[i].id) {
               // find floating point LI
               case MIPS_INS_MTC1:
               {
                  unsigned int rt = insn[i].detail->mips.operands[0].reg;
                  for (int s = i - 1; s >= 0; s--) {
                     if (insn[s].id == MIPS_INS_LUI && insn[s].detail->mips.operands[0].reg == rt) {
                        unsigned int lui_imm = (unsigned int)insn[s].detail->mips.operands[1].imm;
                        lui_imm <<= 16;
                        float f = *((float*)&lui_imm);
                        // link up the LUI with this instruction and the float
                        state->insn_extra[s].linked_insn = i;
                        state->insn_extra[s].linked_float = f;
                        // rewrite LUI instruction to be LI
                        insn[s].id = MIPS_INS_LI;
                        strcpy(insn[s].mnemonic, "li");
                        break;
                     } else if (insn[s].id == MIPS_INS_LW ||
                                insn[s].id == MIPS_INS_LD ||
                                insn[s].id == MIPS_INS_LH ||
                                insn[s].id == MIPS_INS_LHU ||
                                insn[s].id == MIPS_INS_LB ||
                                insn[s].id == MIPS_INS_LBU ||
                                insn[s].id == MIPS_INS_ADDIU ||
                                insn[s].id == MIPS_INS_ADD ||
                                insn[s].id == MIPS_INS_SUB ||
                                insn[s].id == MIPS_INS_SUBU) {
                        unsigned int rd = insn[s].detail->mips.operands[0].reg;
                        if (rt == rd) {
                           break;
                        }
                     } else if (insn[s].id == MIPS_INS_JR &&
                                insn[s].detail->mips.operands[0].reg == MIPS_REG_RA) {
                        // stop looking when previous `jr ra` is hit
                        break;
                     }
                  }
                  break;
               }
               case MIPS_INS_SD:
               case MIPS_INS_SW:
               case MIPS_INS_SH:
               case MIPS_INS_SB:
               case MIPS_INS_LB:
               case MIPS_INS_LBU:
               case MIPS_INS_LD:
               case MIPS_INS_LDL:
               case MIPS_INS_LDR:
               case MIPS_INS_LH:
               case MIPS_INS_LHU:
               case MIPS_INS_LW:
               case MIPS_INS_LWU:
               {
                  unsigned int mem_rs = insn[i].detail->mips.operands[1].mem.base;
                  unsigned int mem_imm = (unsigned int)insn[i].detail->mips.operands[1].mem.disp;
                  link_with_lui(state, i, mem_rs, mem_imm);
                  break;
               }
               case MIPS_INS_ADDIU:
               case MIPS_INS_ORI:
               {
                  unsigned int rd = insn[i].detail->mips.operands[0].reg;
                  unsigned int rs = insn[i].detail->mips.operands[1].reg;
                  int64_t imm = insn[i].detail->mips.operands[2].imm;
                  if (rs == MIPS_REG_ZERO) { // becomes LI
                     insn[i].id = MIPS_INS_LI;
                     strcpy(insn[i].mnemonic, "li");
                     // TODO: is there allocation for this?
                     sprintf(insn[i].op_str, "$%s, %ld", cs_reg_name(handle, rd), imm);
                  } else if (rd == rs) { // only look for LUI if rd and rs are the same
                     link_with_lui(state, i, rs, (unsigned int)imm);
                  }
                  break;
               }
            }
         }
      }
   } else {
      ERROR("Error: Failed to disassemble 0x%X bytes of code at 0x%08X\n", (unsigned int)data_len, vaddr);
   }
}

disasm_state *disasm_state_alloc(void)
{
   disasm_state *dstate = malloc(sizeof(*dstate));
   dstate->label_alloc = 1024;
   dstate->label_count = 0;
   dstate->labels = malloc(sizeof(*dstate->labels) * dstate->label_alloc);
   dstate->instructions = NULL;
   dstate->instruction_count = 0;
   return dstate;
}

void disasm_state_free(disasm_state *state)
{
   if (state) {
      if (state->insn_extra) {
         free(state->insn_extra);
         state->insn_extra = NULL;
      }
      if (state->instructions) {
         cs_free(state->instructions, state->instruction_count);
         state->instructions = NULL;
      }
      cs_close(&state->handle);
   }
}

disasm_state *mipsdisasm_pass1(unsigned char *data, size_t data_len, unsigned int vaddr, asm_syntax syntax, int merge_pseudo, disasm_state *state)
{
   if (state == NULL) {
      state = disasm_state_alloc();
   }
   state->syntax = syntax;

   // collect all branch and jump targets
   disassemble_block(data, data_len, vaddr, state, merge_pseudo);

   // sort labels
   qsort(state->labels, state->label_count, sizeof(state->labels[0]), cmp_label);

   return state;
}

void mipsdisasm_pass2(FILE *out, disasm_state *state)
{
   unsigned int vaddr = state->vaddr;
   int label_idx = 0;
   int label;
   // skip labels before this section
   while ( (label_idx < state->label_count) && (vaddr > state->labels[label_idx].vaddr) ) {
      label_idx++;
   }
   for (int i = 0; i < state->instruction_count; i++) {
      cs_insn *insn = &state->instructions[i];
      cs_mips *mips = &insn->detail->mips;
      // newline between functions
      if (state->insn_extra[i].newline) {
         fprintf(out, "\n");
      }
      // insert all labels at this address
      while ( (label_idx < state->label_count) && (vaddr == state->labels[label_idx].vaddr) ) {
         fprintf(out, "%s:\n", state->labels[label_idx].name);
         label_idx++;
      }
      // TODO: ROM offset?
      fprintf(out, "/* %08X %02X%02X%02X%02X */  ", vaddr, insn->bytes[0], insn->bytes[1], insn->bytes[2], insn->bytes[3]);
      if (cs_insn_group(state->handle, insn, MIPS_GRP_JUMP)) {
         fprintf(out, "%-5s ", insn->mnemonic);
         for (int o = 0; o < mips->op_count; o++) {
            if (o > 0) {
               fprintf(out, ", ");
            }
            switch (mips->operands[o].type) {
               case MIPS_OP_REG:
                  fprintf(out, "$%s", cs_reg_name(state->handle, mips->operands[o].reg));
                  break;
               case MIPS_OP_IMM:
               {
                  unsigned int branch_target = (unsigned int)mips->operands[o].imm;
                  label = label_find(state, branch_target);
                  fprintf(out, state->labels[label].name);
                  break;
               }
               default:
                  break;
            }
         }
         fprintf(out, "\n");
      } else if (insn->id == MIPS_INS_JAL || insn->id == MIPS_INS_BAL) {
         unsigned int jal_target = (unsigned int)mips->operands[0].imm;
         label = label_find(state, jal_target);
         fprintf(out, "%-5s ", insn->mnemonic);
         if (label >= 0) {
            fprintf(out, "%s\n", state->labels[label].name);
         }
      } else if (insn->id == MIPS_INS_MTC0 || insn->id == MIPS_INS_MFC0) {
         // workaround bug in capstone/LLVM
         unsigned char rd;
         // 31-24 23-16 15-8 7-0
         // 0     1     2    3
         //       31-26  25-21 20-16 15-11 10-0
         // mfc0: 010000 00000   rt    rd  00000000000
         // mtc0: 010000 00100   rt    rd  00000000000
         //       010000 00100 00000 11101 000 0000 0000
         // rt = insn->bytes[1] & 0x1F;
         rd = (insn->bytes[2] & 0xF8) >> 3;
         fprintf(out, "%-5s $%s, $%d\n", insn->mnemonic,
                 cs_reg_name(state->handle, mips->operands[0].reg), rd);
      } else {
         int linked_insn = state->insn_extra[i].linked_insn;
         if (linked_insn >= 0) {
            if (insn->id == MIPS_INS_LI) {
               // assume this is LUI converted to LI for matched MTC1
               fprintf(out, "%-5s ", insn->mnemonic);
               switch (state->syntax) {
                  case ASM_GAS:
                     fprintf(out, "$%s, 0x%04X0000 # %f\n",
                           cs_reg_name(state->handle, mips->operands[0].reg),
                           (unsigned int)mips->operands[1].imm,
                           state->insn_extra[i].linked_float);
                     break;
                  case ASM_ARMIPS:
                     fprintf(out, "$%s, 0x%04X0000 // %f\n",
                           cs_reg_name(state->handle, mips->operands[0].reg),
                           (unsigned int)mips->operands[1].imm,
                           state->insn_extra[i].linked_float);
                     break;
                  // TODO: this is ideal, but it doesn't work exactly for all floats since some emit imprecise float strings
                  /*
                     fprintf(out, "$%s, %f // 0x%04X\n",
                           cs_reg_name(state->handle, mips->operands[0].reg),
                           state->insn_extra[i].linked_float,
                           (unsigned int)mips->operands[1].imm);
                     break;
                   */
               }
            } else if (insn->id == MIPS_INS_LUI) {
               label = label_find(state, state->insn_extra[i].linked_value);
               // assume matched LUI with ADDIU/LW/SW etc.
               switch (state->syntax) {
                  case ASM_GAS:
                     // TODO: this isn't exactly true for LI -> LUI/ORI pair
                     fprintf(out, "%-5s $%s, %%hi(%s)\n", insn->mnemonic,
                           cs_reg_name(state->handle, mips->operands[0].reg),
                           state->labels[label].name);
                     break;
                  case ASM_ARMIPS:
                     switch (state->instructions[linked_insn].id) {
                        case MIPS_INS_ADDIU:
                           fprintf(out, "%-5s $%s, %s // %s %s\n", "la.u",
                                 cs_reg_name(state->handle, mips->operands[0].reg),
                                 state->labels[label].name,
                                 insn->mnemonic, insn->op_str);
                           break;
                        case MIPS_INS_ORI:
                           fprintf(out, "%-5s $%s, 0x%08X // %s %s\n", "li.u",
                                 cs_reg_name(state->handle, mips->operands[0].reg),
                                 state->insn_extra[i].linked_value,
                                 insn->mnemonic, insn->op_str);
                           break;
                        default: // LW/SW/etc.
                           fprintf(out, "%-5s $%s, hi(%s)\n", insn->mnemonic,
                                 cs_reg_name(state->handle, mips->operands[0].reg),
                                 state->labels[label].name);
                           break;
                     }
                     break;
               }
            } else if (insn->id == MIPS_INS_ADDIU || insn->id == MIPS_INS_ORI) {
               label = label_find(state, state->insn_extra[i].linked_value);
               switch (state->syntax) {
                  case ASM_GAS:
                     // TODO: this isn't exactly true for LI -> LUI/ORI pair
                     fprintf(out, "%-5s $%s, %%lo(%s)\n", insn->mnemonic,
                           cs_reg_name(state->handle, mips->operands[0].reg),
                           state->labels[label].name);
                     break;
                  case ASM_ARMIPS:
                     switch (insn->id) {
                        case MIPS_INS_ADDIU:
                           fprintf(out, "%-5s $%s, %s // %s %s\n", "la.l",
                                 cs_reg_name(state->handle, mips->operands[0].reg),
                                 state->labels[label].name,
                                 insn->mnemonic, insn->op_str);
                           break;
                        case MIPS_INS_ORI:
                           fprintf(out, "%-5s $%s, 0x%08X // %s %s\n", "li.l",
                                 cs_reg_name(state->handle, mips->operands[0].reg),
                                 state->insn_extra[i].linked_value,
                                 insn->mnemonic, insn->op_str);
                           break;
                     }
                     break;
               }
            } else {
               label = label_find(state, state->insn_extra[i].linked_value);
               fprintf(out, "%-5s $%s, %slo(%s)($%s)\n", insn->mnemonic,
                     cs_reg_name(state->handle, mips->operands[0].reg),
                     state->syntax == ASM_GAS ? "%" : "",
                     state->labels[label].name,
                     cs_reg_name(state->handle, mips->operands[1].reg));
            }
         } else {
            fprintf(out, "%-5s %s\n", insn->mnemonic, insn->op_str);
         }
      }
      vaddr += 4;
   }
}

const char *disasm_get_version(void)
{
   static char version[32];
   int major, minor;
   (void)cs_version(&major, &minor);
   // TODO: manually keeping track of capstone revision number
   sprintf(version, "capstone %d.%d.4", major, minor);
   return version;
}

#ifdef MIPSDISASM_STANDALONE
typedef struct
{
   unsigned int start;
   unsigned int length;
   unsigned int vaddr;
} range;

typedef struct
{
   range *ranges;
   int range_count;
   unsigned int vaddr;
   char *input_file;
   char *output_file;
   int merge_pseudo;
   asm_syntax syntax;
} arg_config;

static arg_config default_args =
{
   NULL, // ranges
   0,    // range_count
   0x0,  // vaddr
   NULL, // input_file
   NULL, // output_file
   0,    // merge_pseudo
   ASM_GAS, // GNU as
};

static void print_usage(void)
{
   ERROR("Usage: mipsdisasm [-o OUTPUT] [-p] [-s ASSEMBLER] [-v] ROM [RANGES]\n"
         "\n"
         "mipsdisasm v" MIPSDISASM_VERSION ": MIPS disassembler\n"
         "\n"
         "Optional arguments:\n"
         " -o OUTPUT    output filename (default: stdout)\n"
         " -p           emit pseudoinstructions for related instructions\n"
         " -s SYNTAX    assembler syntax to use [gas, armips] (default: gas)\n"
         " -v           verbose progress output\n"
         "\n"
         "Arguments:\n"
         " FILE         input binary file to disassemble\n"
         " [RANGES]     optional list of ranges (default: entire input file)\n"
         "              format: <VAddr>:[<Start>-<End>] or <VAddr>:[<Start>+<Length>]\n"
         "              example: 0x80246000:0x1000-0x0E6258\n");
   exit(EXIT_FAILURE);
}

void range_parse(range *r, const char *arg)
{
   char *colon = strchr(arg, ':');
   r->vaddr = strtoul(arg, NULL, 0);
   if (colon) {
      char *minus = strchr(colon+1, '-');
      char *plus = strchr(colon+1, '+');
      r->start = strtoul(colon+1, NULL, 0);
      if (minus) {
         r->length = strtoul(minus+1, NULL, 0) - r->start;
      } else if (plus) {
         r->length = strtoul(plus+1, NULL, 0);
      }
   }
}

// parse command line arguments
static void parse_arguments(int argc, char *argv[], arg_config *config)
{
   int file_count = 0;
   if (argc < 2) {
      print_usage();
      exit(1);
   }
   config->ranges = malloc(argc / 2 * sizeof(*config->ranges));
   config->range_count = 0;
   for (int i = 1; i < argc; i++) {
      if (argv[i][0] == '-') {
         switch (argv[i][1]) {
            case 'o':
               if (++i >= argc) {
                  print_usage();
               }
               config->output_file = argv[i];
               break;
            case 'p':
               config->merge_pseudo = 1;
               break;
            case 's':
            {
               if (++i >= argc) {
                  print_usage();
               }
               if ((0 == strcasecmp("gas", argv[i])) ||
                   (0 == strcasecmp("gnu", argv[i]))) {
                  config->syntax = ASM_GAS;
               } else if (0 == strcasecmp("armips", argv[i])) {
                  config->syntax = ASM_ARMIPS;
               } else {
                  print_usage();
               }
               break;
            }
            case 'v':
               g_verbosity = 1;
               break;
            default:
               print_usage();
               break;
         }
      } else {
         if (file_count == 0) {
            config->input_file = argv[i];
         } else {
            range_parse(&config->ranges[config->range_count], argv[i]);
            config->range_count++;
         }
         file_count++;
      }
   }
   if (file_count < 1) {
      print_usage();
   }
}

int main(int argc, char *argv[])
{
   arg_config args;
   long file_len;
   disasm_state *state;
   unsigned char *data;
   FILE *out;

   // load defaults and parse arguments
   out = stdout;
   args = default_args;
   parse_arguments(argc, argv, &args);

   // read input file
   INFO("Reading input file '%s'\n", args.input_file);
   file_len = read_file(args.input_file, &data);
   if (file_len <= 0) {
      ERROR("Error reading input file '%s'\n", args.input_file);
      return EXIT_FAILURE;
   }

   // if specified, open output file
   if (args.output_file != NULL) {
      INFO("Opening output file '%s'\n", args.output_file);
      out = fopen(args.output_file, "w");
      if (out == NULL) {
         ERROR("Error opening output file '%s'\n", args.output_file);
         return EXIT_FAILURE;
      }
   }

   // if no ranges specified or if only vaddr specified, add one of entire input file
   if (args.range_count < 1 || (args.range_count == 1 && args.ranges[0].length == 0)) {
      if (args.range_count < 1) {
         args.ranges[0].vaddr = 0;
      }
      args.ranges[0].start = 0;
      args.ranges[0].length = file_len;
      args.range_count = 1;
   }

   // assembler header output
   switch (args.syntax) {
      case ASM_GAS:
         fprintf(out, ".set noat      # allow manual use of $at\n");
         fprintf(out, ".set noreorder # don't insert nops after branches\n\n");
         break;
      case ASM_ARMIPS:
      {
         char output_binary[FILENAME_MAX];
         if (args.output_file == NULL) {
            strcpy(output_binary, "test.bin");
         } else {
            const char *base = basename(args.output_file);
            generate_filename(base, output_binary, "bin");
         }
         fprintf(out, ".n64\n");
         fprintf(out, ".create \"%s\", 0x%08X\n\n", output_binary, 0);
         break;
      }
      default:
         break;
   }

   state = disasm_state_alloc();
   for (int i = 0; i < args.range_count; i++) {
      range *r = &args.ranges[i];
      INFO("Disassembling range 0x%X-0x%X at 0x%08X\n", r->start, r->start + r->length, r->vaddr);

      fprintf(out, ".headersize 0x%08X\n\n", r->vaddr);
      (void)mipsdisasm_pass1(&data[r->start], r->length, r->vaddr, args.syntax, args.merge_pseudo, state);

      if (args.syntax == ASM_ARMIPS) {
         for (int j = 0; j < state->label_count; j++) {
            unsigned int vaddr = state->labels[j].vaddr;
            if (vaddr < r->vaddr || vaddr > r->vaddr + r->length) {
               fprintf(out, ".definelabel %s, 0x%08X\n", state->labels[j].name, vaddr);
            }
         }
      }
      fprintf(out, "\n");

      // second pass, generate output
      // TODO: either call pass2 with vaddr/length for each section or call once outside of `for` loop
      mipsdisasm_pass2(out, state);
   }
   disasm_state_free(state);

   // assembler footer output
   switch (args.syntax) {
      case ASM_ARMIPS:
         fprintf(out, "\n.close\n");
         break;
      default:
         break;
   }

   free(data);

   return EXIT_SUCCESS;
}

#endif // MIPSDISASM_STANDALONE
