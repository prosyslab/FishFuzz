/*
   FishFuzz based american fuzzy lop++ - wrapper for 
   -------------------------------------------

   Originally written by Michal Zalewski

   AFL++ maintained by Marc Heuse <mh@mh-sec.de>,
                        Heiko Eißfeldt <heiko.eissfeldt@hexco.de> and
                        Andrea Fioraldi <andreafioraldi@gmail.com>
   FF_AFL++ maintained by Han Zheng <kdsjzh@gmail.com>

   Copyright 2016, 2017 Google Inc. All rights reserved.
   Copyright 2019-2022 AFLplusplus Project. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     https://www.apache.org/licenses/LICENSE-2.0

   Try to merge the entire compilation process into one wrapper
      Step 1: original compiling procedure in lto mode, generating .bc file
      Step 2: do a preliminary analysis for .bc file to generate the CFG/Sanitizer location info...
      Step 3: callgraph generation and static distance map calculation
      Step 4: generating binary with function/Sanitizer location instrumentations for final fuzzing 

 */


#include "config.h"
#include "types.h"
#include "debug.h"
#include "alloc-inl.h"


#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/stat.h>




u8 *obj_path, *ff_driver_name, *ff_driver_path, *tmp_path;
u8 *log_path;
// char ** ff_driver_args;
FILE *plog;
int log_fd;


/* Dry run for dis_calc to avoid it broken or not exist */

u8 dry_run_bin(u8* bin_path, u8** parm, u8 write_log) {

  if (access(bin_path, R_OK)) FATAL("Oops, %s not found!", bin_path);

  /* just skip the check if it's already done */
  if (!write_log) return 1;

  parm[0] = bin_path;

  pid_t pid = fork();

  if (pid == -1) FATAL("Oops, failed to fork.");
  else if (pid == 0) {
      
    ACTF("Try if %s works...", bin_path);

    /* close stdout to disable dryrun's output, use log file later */
    dup2(log_fd, 1);
    execvp(bin_path, (char**)parm);

    PFATAL("Oops, %s broken!", bin_path);

  } else {

    int status;
    waitpid(pid, &status, 0);

  }

  return 1;
  
}

/* Try to find the runtime libraries & distance calculator. If that fails, abort. */

static void find_obj(u8* argv0, u8 write_log) {

  u8 *afl_path = getenv("AFL_PATH");
  u8 *slash, *tmp1, *tmp2, *tmp3, *tmp4;

  u8 **parm1, **parm2;
  parm1 = ck_alloc(sizeof(u8*) * 5);
  parm1[1] = "-h"; parm1[4] = NULL;
  parm2 = ck_alloc(sizeof(u8*) * 5);
  parm2[1] = "--version"; parm2[4] = NULL;


  if (afl_path) {

    tmp1 = alloc_printf("%s/afl-compiler-rt.o", afl_path);
    tmp2 = alloc_printf("%s/dyncfg/dis_calc", afl_path);
    tmp3 = alloc_printf("%s/afl-fish-fast++", afl_path);
    tmp4 = alloc_printf("%s/afl-clang-fast++", afl_path);

    if (!access(tmp1, R_OK) && dry_run_bin(tmp2, parm1, write_log) &&
        dry_run_bin(tmp3, parm2, write_log) && dry_run_bin(tmp4, parm2, write_log)) {
      obj_path = afl_path;
      ck_free(tmp1); ck_free(tmp2);
      ck_free(tmp3); ck_free(tmp4);
      ck_free(parm1); ck_free(parm2);
      return;
    }

    ck_free(tmp1); ck_free(tmp2);
    ck_free(tmp3); ck_free(tmp4);

  }

  slash = strrchr(argv0, '/');

  if (slash) {

    u8 *dir;

    *slash = 0;
    dir = ck_strdup(argv0);
    *slash = '/';

    tmp1 = alloc_printf("%s/afl-compiler-rt.o", dir);
    tmp2 = alloc_printf("%s/dyncfg/dis_calc", dir);
    tmp3 = alloc_printf("%s/afl-fish-fast++", dir);
    tmp4 = alloc_printf("%s/afl-clang-fast++", dir);

    if (!access(tmp1, R_OK) && dry_run_bin(tmp2, parm1, write_log) &&
        dry_run_bin(tmp3, parm2, write_log) && dry_run_bin(tmp4, parm2, write_log)) {
      obj_path = dir;
      ck_free(tmp1); ck_free(tmp2);
      ck_free(tmp3); ck_free(tmp4);
      return;
    }

    ck_free(tmp1); ck_free(tmp2);
    ck_free(tmp3); ck_free(tmp4);
    ck_free(dir);

  }

  if (!access(AFL_PATH "/afl-compiler-rt.o", R_OK) &&
      dry_run_bin(AFL_PATH "/dyncfg/dis_calc", parm1, write_log) &&
      dry_run_bin(AFL_PATH "/afl-fish-fast++", parm2, write_log) &&
      dry_run_bin(AFL_PATH "/afl-clang-fast++", parm2, write_log)) {
    obj_path = AFL_PATH;
    ck_free(parm1); ck_free(parm2);
    return;
  }

  ck_free(parm1); ck_free(parm2);

  FATAL("Fishfuzz utils not available, please check if the following objects/binaries/soft links works : 'afl-fish-pass.so', 'afl-compiler-rt.o', 'dyncfg/dis_calc' or 'afl-fish-fast++'. Please set AFL_PATH");

}

u8 open_log(u8* driver_name) {

  u8  write_log = 1;

  u8* ff_dir = alloc_printf("/tmp/fishfuzz/");
  if (access(ff_dir, F_OK) == -1) {
    
    if (mkdir(ff_dir, 0700)) PFATAL("Unable to create log dir '%s'", ff_dir);
  
  }

  log_path = alloc_printf("%s/%s", ff_dir, driver_name);

  /* if already exists, do not write log */
  if (access(log_path, F_OK) != -1) write_log = 0;

  plog = fopen(log_path, "a");

  if (!plog) PFATAL("Failed open log file %s", log_path);

  log_fd = fileno(plog);

  return write_log;

}

/* Check the following environment variables and binary */

void preliminary_check(u8* argv0) {

  ff_driver_name = getenv("FF_DRIVER_NAME");
  if (ff_driver_name == NULL) {

    PFATAL("Please specify the driver name (e.g. tiff2pdf for libtiff).");

  }
  
  u8 write_log = open_log(ff_driver_name);

  find_obj(argv0, write_log);

}

void fork_and_exec(u8 **parm, u8 ff_steps, u8 is_cxx, u8 is_linking) {

  if (ff_steps == 1) write(log_fd, "---------------FF Compilation-------------\n", 45);

  for (u32 i = 0; parm[i]; i ++) printf("%s ", parm[i]);
  printf("\n");


  /* Start Execution */
  pid_t pid = fork();

  if (pid == -1) FATAL("Oops, failed to fork.");
  else if (pid == 0) {
      

    /* close stdout to disable dryrun's output, use log file later */
    if (!ff_steps) dup2(log_fd, 1);
    execvp(parm[0], (char**)parm);

    PFATAL("Oops, compilation failed!");

  } else {

    int status;
    waitpid(pid, &status, 0);

  }
  if (ff_steps) {
    if (is_cxx) OKF("ff-all-in-one++ %s...", (is_linking) ? (u8*)"linking" : (u8*)"compiling");
    else OKF("ff-all-in-one %s...", (is_linking) ? (u8*)"linking" : (u8*)"compiling");
  } else {
    OKF("ff-all-in-one processing ff steps %d...", ff_steps);
  }

}

void edit_and_exec(int argc, char **argv) {

  u8 **new_parm = (u8 **)malloc(sizeof(u8*) * 128 + 1);

  u8 is_linking = 0, is_obj = 0, has_asan = 0, is_cxx = 0;
  
  for (u32 i = 1; i < argc; i ++) {
    
    if (!strcmp(argv[i], "-o") && !is_obj) is_linking = 1;

    if (!strcmp(argv[i], "-c")) {is_obj = 1; is_linking = 0;}

    if (!strcmp(argv[i], "-fsanitize=address")) has_asan = 1;
  
  }

  u32 new_parm_cnt = 0;

  /* in current stage you should use clang/clang++ */
  u8 *name = strrchr(argv[0], '/');
  if (!name) name = argv[0]; else name++;

  if (!strcmp(name, "ff-all-in-one++")) {
  
    new_parm[new_parm_cnt ++] = (u8*)"clang++";
    is_cxx = 1;
  
  } else {
  
    new_parm[new_parm_cnt ++] = (u8*)"clang";
  
  }

  if (!has_asan) {
    
    new_parm[new_parm_cnt ++] = (u8*)"-fsanitize=address";

  }
  
  new_parm[new_parm_cnt ++] = (u8*)"-flto";

  if (is_linking) {

    new_parm[new_parm_cnt ++] = (u8*)"-fuse-ld=gold";
    new_parm[new_parm_cnt ++] = (u8*)"-Wl,-plugin-opt=save-temps";

  }
  for (u32 i = 1; i < argc; i ++) {
  
    new_parm[new_parm_cnt ++] = argv[i];
    
  }
  new_parm[new_parm_cnt] = NULL;


  /* Start Execution */
  fork_and_exec(new_parm, 0, is_cxx, is_linking);

}

u8* get_asan_library() {

  // find `llvm-config --libdir` -name libclang_rt.asan-*.a|head -n 1
  FILE* fp = popen("find `llvm-config --libdir` -name libclang_rt.asan-*.a", "r");
  u8* asan_lib_path = (u8*)malloc(sizeof(u8) * 256);

  if (fp) {

    while (fgets(asan_lib_path, 256, fp) != NULL) {

      if (!strstr(asan_lib_path, "asan-preinit-"))  
        break;
  
    }

    char *line_end = strchr(asan_lib_path, '\n');
    if (line_end) *line_end= NULL;

    pclose(fp);

    return asan_lib_path;

  }
  PFATAL("Cannot find asan static library : %s", strerror(errno));

}

/* check if it's generating target binary, saving the argv as well */
u8 is_target_gen(int argc, char **argv) {

  for (u32 i = 1; i < argc - 1; i ++) {

    if (!strcmp(argv[i], "-o")) {

      u8 *slash = strrchr(argv[i + 1], '/');

      u8 bpath[0x100 + 1];
      if (getcwd(bpath, sizeof(bpath)) == NULL) {

        PFATAL("failed get current path.");
      
      } // else ACTF("current path is %s, next arg is %s.", bpath, argv[i + 1]);

      u8 is_target = 0;
      
      if (slash) {

        if (!strcmp(slash + 1, ff_driver_name)) {

          is_target = 1;
          // *slash = 0;
          // ff_driver_path = alloc_printf("%s%s", bpath, ck_strdup(argv[i + 1]));
          // *slash = '/';
          ff_driver_path = ck_strdup(argv[i + 1]);

        }
      
      } else {
        
        if (!strcmp(argv[i + 1], ff_driver_name)) {

          is_target = 1;
          // ff_driver_path = alloc_printf("%s/%s", bpath, ck_strdup(argv[i + 1]));
          ff_driver_path = ck_strdup(argv[i + 1]);
        
        }
      
      }
      

      if (is_target) {

        // ff_driver_args = (char **)malloc(sizeof(u8*) * (argc + 1));
        // for (u32 j = 0; j < argc; j ++) {
        //   ff_driver_args[j] = ck_strdup(argv[j]);
        // }
        // ff_driver_args[argc] = 0;

        tmp_path = alloc_printf("%s/TEMP_%s", bpath, ff_driver_name);

        if (access(tmp_path, F_OK) == -1) {
    
          if (mkdir(tmp_path, 0700)) PFATAL("Unable to create log dir '%s'", tmp_path);
        
        }
        
        return 1;

      }

      

    }
  
  }

  return 0;
}


/* generating log files in temporary dir and run pass */
void preprocessing() {

  u8 **pass_parm1 = (u8 **)malloc(sizeof(u8*) * 128 + 1),
     **pass_parm2 = (u8 **)malloc(sizeof(u8*) * 128 + 1),
     **pass_parm3 = (u8 **)malloc(sizeof(u8*) * 128 + 1);
  
  u32 parm1_cnt = 0, parm2_cnt = 0, parm3_cnt = 0;

  ACTF("Now Working on FF preprocessing...");
  
  // -load $FISHFUZZ/afl-fish-pass.so -test -outdir=$TMP_DIR -pmode=rename $BC_PATH$PROJ_NAME.0.5.precodegen.bc -o $BC_PATH$PROJ_NAME.rename.bc
  pass_parm1[parm1_cnt ++] = (u8*) "opt";
  pass_parm1[parm1_cnt ++] = (u8*) "-load";
  pass_parm1[parm1_cnt ++] = alloc_printf("%s/afl-fish-pass.so", obj_path);
  pass_parm1[parm1_cnt ++] = (u8*) "-test";
  pass_parm1[parm1_cnt ++] = alloc_printf("-outdir=%s", tmp_path);
  pass_parm1[parm1_cnt ++] = (u8*) "-pmode=rename";
  pass_parm1[parm1_cnt ++] = alloc_printf("%s.0.5.precodegen.bc", ff_driver_path);
  pass_parm1[parm1_cnt ++] = (u8*) "-o";
  pass_parm1[parm1_cnt ++] = alloc_printf("%s.rename.bc", ff_driver_path);
  pass_parm1[parm1_cnt] = NULL;
  fork_and_exec(pass_parm1, 1, 0, 0);

  // -load $FISHFUZZ/SanitizerCoveragePCGUARD.so -cov $BC_PATH$PROJ_NAME.rename.bc -o $BC_PATH$PROJ_NAME.cov.bc
  pass_parm2[parm2_cnt ++] = (u8*) "opt";
  pass_parm2[parm2_cnt ++] = (u8*) "-load";
  pass_parm2[parm2_cnt ++] = alloc_printf("%s/SanitizerCoveragePCGUARD.so", obj_path);
  pass_parm2[parm2_cnt ++] = (u8*) "-cov";
  pass_parm2[parm2_cnt ++] = alloc_printf("%s.rename.bc", ff_driver_path);
  pass_parm2[parm2_cnt ++] = (u8*) "-o";
  pass_parm2[parm2_cnt ++] = alloc_printf("%s.cov.bc", ff_driver_path);
  pass_parm2[parm2_cnt] = NULL;
  fork_and_exec(pass_parm2, 2, 0, 0);

  // -load $FISHFUZZ/afl-fish-pass.so -test -outdir=$TMP_DIR -pmode=aonly $BC_PATH$PROJ_NAME.rename.bc -o $BC_PATH$PROJ_NAME.temp.bc
  pass_parm3[parm3_cnt ++] = (u8*) "opt";
  pass_parm3[parm3_cnt ++] = (u8*) "-load";
  pass_parm3[parm3_cnt ++] = alloc_printf("%s/afl-fish-pass.so", obj_path);
  pass_parm3[parm3_cnt ++] = (u8*) "-test";
  pass_parm3[parm3_cnt ++] = alloc_printf("-outdir=%s", tmp_path);
  pass_parm3[parm3_cnt ++] = (u8*) "-pmode=aonly";
  pass_parm3[parm3_cnt ++] = alloc_printf("%s.rename.bc", ff_driver_path);
  pass_parm3[parm3_cnt ++] = (u8*) "-o";
  pass_parm3[parm3_cnt ++] = alloc_printf("%s.temp.bc", ff_driver_path);
  pass_parm3[parm3_cnt] = NULL;
  fork_and_exec(pass_parm3, 3, 0, 0);
  
  free(pass_parm1);
  free(pass_parm2);
  free(pass_parm3);

}


/* generating cg and calculating distance via python script */
void distance_calc() {

  u8 **cg_parm1 = (u8 **)malloc(sizeof(u8*) * 128 + 1),
     **cg_parm2 = (u8 **)malloc(sizeof(u8*) * 128 + 1),
     **distance_parm = (u8 **)malloc(sizeof(u8*) * 128 + 1);
  u32 dist_parm_cnt = 0, cg_parm_cnt = 0;

  ACTF("Now calculating static distance ...");

  // opt -dot-callgraph $BC_PATH$PROJ_NAME.cov.bc && mv $BC_PATH$PROJ_NAME.cov.bc.callgraph.dot $TMP_DIR/dot-files/callgraph.dot
  cg_parm1[cg_parm_cnt ++] = (u8*)"opt";
  cg_parm1[cg_parm_cnt ++] = (u8*)"-dot-callgraph";
  cg_parm1[cg_parm_cnt ++] = alloc_printf("%s.cov.bc", ff_driver_path);
  cg_parm1[cg_parm_cnt] = NULL;
  fork_and_exec(cg_parm1, 4, 0, 0); cg_parm_cnt = 0;
  
  cg_parm2[cg_parm_cnt ++] = (u8*)"mv";
  cg_parm2[cg_parm_cnt ++] = alloc_printf("%s.cov.bc.callgraph.dot", ff_driver_path);
  cg_parm2[cg_parm_cnt ++] = alloc_printf("%s/dot-files/callgraph.dot", tmp_path);
  cg_parm2[cg_parm_cnt] = NULL;
  fork_and_exec(cg_parm2, 4, 0, 0);

  distance_parm[dist_parm_cnt ++] = (u8*)"python3";
  distance_parm[dist_parm_cnt ++] = alloc_printf("%s/scripts/gen_initial_distance.py", obj_path);
  distance_parm[dist_parm_cnt ++] = tmp_path;
  distance_parm[dist_parm_cnt] = NULL;
  fork_and_exec(distance_parm, 4, 0, 0);

  free(distance_parm);
  free(cg_parm1);

}

void target_generation(int argc, char **argv) {

  u8 **target_parm = (u8 **)malloc(sizeof(u8*) * 128 + 1);
  u32 parm_cnt = 0;

  ACTF("Now generating final target ...");

  // -pmode=fonly -funcid=$TMP_DIR/funcid.csv -outdir=$TMP_DIR $BC_PATH$PROJ_NAME.cov.bc -o $BC_PATH$PROJ_NAME.fuzz $ASAN_LIBS
  u8 *name = strrchr(argv[0], '/');
  if (!name) name = argv[0]; else name++;
  if (!strcmp(name, "ff-all-in-one++")) {
  
    target_parm[parm_cnt ++] = alloc_printf("%s/afl-fish-fast++", obj_path);
  
  } else {

    target_parm[parm_cnt ++] = alloc_printf("%s/afl-fish-fast", obj_path);

  }
  
  target_parm[parm_cnt ++] = (u8*) "-pmode=fonly";
  target_parm[parm_cnt ++] = alloc_printf("-funcid=%s/funcid.csv", tmp_path);
  target_parm[parm_cnt ++] = alloc_printf("-outdir=%s", tmp_path);
  target_parm[parm_cnt ++] = alloc_printf("%s.cov.bc", ff_driver_path);
  target_parm[parm_cnt ++] = (u8*) "-o";
  target_parm[parm_cnt ++] = alloc_printf("%s.fuzz", ff_driver_path);

  // searching for additional library
  for (int i = 0; i < argc; i ++) {

    // start with -lxxx
    if (strstr(argv[i], (u8*)"-l") == argv[i]) target_parm[parm_cnt ++] = argv[i];
    
  }
  // add asan static library, together with its runtime library -ldl -lm -lpthread
  target_parm[parm_cnt ++] = (u8*) "-ldl";
  target_parm[parm_cnt ++] = (u8*) "-lm";
  target_parm[parm_cnt ++] = (u8*) "-lpthread";
  target_parm[parm_cnt ++] = get_asan_library();
  
  target_parm[parm_cnt] = NULL;
  fork_and_exec(target_parm, 3, 0, 0);

  free(target_parm);


}



int main(int argc, char** argv) {

  if (isatty(2) && !getenv("AFL_QUIET")) {

#ifdef USE_TRACE_PC
    SAYF(cCYA "fishfuzz-all-in-one [tpcg] " cBRI VERSION  cRST " by <kdsjzh@gmail.com>\n");
#else
    SAYF(cCYA "fishfuzz-all-in-one " cBRI VERSION  cRST " by <kdsjzh@gmail.com>\n");
#endif /* ^USE_TRACE_PC */

  }

  if (argc < 2) {

    SAYF("\n"
         "This is a helper application for FishFuzz. It serves as a drop-in replacement\n"
         "for clang, letting you recompile third-party code with the required runtime\n"
         "instrumentation. A common use pattern would be one of the following:\n\n"

         "  FF_DRIVER_NAME=xx CC=%s/ff-all-in-one ./configure\n"
         "  FF_DRIVER_NAME=xx CXX=%s/ff-all-in-one ./configure\n\n"

         "In contrast to the traditional afl-clang tool, this version is implemented as\n"
         "an LLVM pass and tends to offer improved performance with slow programs.\n\n"

         "You can specify custom next-stage toolchain via AFL_CC and AFL_CXX. Setting\n"
         "AFL_HARDEN enables hardening optimizations in the compiled code.\n\n",
         BIN_PATH, BIN_PATH);

    exit(1);

  }

  preliminary_check(argv[0]);

  /* if didn't find -o FF_DRIVER_NAME, then performs as original 
     (with asan and lto enabled, generate binary with temporary bc saved)
     else we should start preprocessing, cg generation, and instrument
     for the final binary */

  edit_and_exec(argc, argv);

  if (is_target_gen(argc, argv)) {

    // OKF("Now we're generating target driver in %s.", ff_driver_path);
    // for llvm 14 or higher, we need to add -enable-new-pm=0 argument for opt 

    preprocessing();
    distance_calc();
    target_generation(argc, argv);

  }  

  return 0;

}