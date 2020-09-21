/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2008 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#define __STDC_LIMIT_MACROS
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <mach/mach.h>

#include "dyld2.h"
#include "dyldSyscallInterface.h"
#include "MachOAnalyzer.h"
#include "Tracing.h"

// from libc.a
extern "C" void mach_init();
extern "C" void __guard_setup(const char* apple[]);


// from dyld_debugger.cpp
extern void syncProcessInfo();

const dyld::SyscallHelpers* gSyscallHelpers = NULL;


//
//  Code to bootstrap dyld into a runnable state
//
//



namespace dyldbootstrap {


// currently dyld has no initializers, but if some come back, set this to non-zero
#define DYLD_INITIALIZER_SUPPORT  0


#if DYLD_INITIALIZER_SUPPORT

typedef void (*Initializer)(int argc, const char* argv[], const char* envp[], const char* apple[]);

extern const Initializer  inits_start  __asm("section$start$__DATA$__mod_init_func");
extern const Initializer  inits_end    __asm("section$end$__DATA$__mod_init_func");

//
// For a regular executable, the crt code calls dyld to run the executables initializers.
// For a static executable, crt directly runs the initializers.
// dyld (should be static) but is a dynamic executable and needs this hack to run its own initializers.
// We pass argc, argv, etc in case libc.a uses those arguments
//
static void runDyldInitializers(int argc, const char* argv[], const char* envp[], const char* apple[])
{
	for (const Initializer* p = &inits_start; p < &inits_end; ++p) {
		(*p)(argc, argv, envp, apple);
	}
}
#endif // DYLD_INITIALIZER_SUPPORT


//
// On disk, all pointers in dyld's DATA segment are chained together.
// They need to be fixed up to be real pointers to run.

// dyld重定位
static void rebaseDyld(const dyld3::MachOLoaded* dyldMH)
{
    // walk all fixups chains and rebase dyld
    const dyld3::MachOAnalyzer* ma = (dyld3::MachOAnalyzer*)dyldMH;
    assert(ma->hasChainedFixups());
    //  这个其实就是 ALSR , 说白了就是通过一个随机值 ( 也就是我们这里的 slide ) 来实现地址空间配置随机加载 .
    uintptr_t slide = (long)ma; // all fixup chain based images have a base address of zero, so slide == load address
    
    __block Diagnostics diag;
    ma->withChainStarts(diag, 0, ^(const dyld_chained_starts_in_image* starts) {
        ma->fixupAllChainedFixups(diag, starts, slide, dyld3::Array<const void*>(), nullptr);
    });
    diag.assertNoError();

    // now that rebasing done, initialize mach/syscall layer
    
    // mach消息初始化, 允许 dyld 使用 mach 消息传递
    mach_init();

    // <rdar://47805386> mark __DATA_CONST segment in dyld as read-only (once fixups are done)
    ma->forEachSegment(^(const dyld3::MachOFile::SegmentInfo& info, bool& stop) {
        if ( info.readOnlyData ) {
            ::mprotect(((uint8_t*)(dyldMH))+info.vmAddr, (size_t)info.vmSize, VM_PROT_READ);
        }
    });
}



//
//  This is code to bootstrap dyld.  This work in normally done for a program by dyld and crt.
//  In dyld we have to do this manually.
//

/**

dyldbootstrap::start()函数中做了很多dyld初始化相关的工作，包括：

rebaseDyld() dyld重定位。
mach_init() mach消息初始化。
__guard_setup() 栈溢出保护。
初始化工作完成后，此函数调用到了dyld::_main()，再将返回值传递给__dyld_start去调用真正的main()函数。在dyldInitialization.cpp文件中可以找到dyldbootstrap::start()函数的实现如下：

*/
uintptr_t start(const dyld3::MachOLoaded* appsMachHeader, int argc, const char* argv[],
				const dyld3::MachOLoaded* dyldsMachHeader, uintptr_t* startGlue)
{

    // Emit kdebug tracepoint to indicate dyld bootstrap has started <rdar://46878536>
    dyld3::kdebug_trace_dyld_marker(DBG_DYLD_TIMING_BOOTSTRAP_START, 0, 0, 0, 0);

	// if kernel had to slide dyld, we need to fix up load sensitive locations
	// we have to do this before using any global variables
    
    // dyld重定位(包含ASLR、 mach_init() 、mach消息初始化)
    rebaseDyld(dyldsMachHeader);

	// kernel sets up env pointer to be just past end of agv array
	const char** envp = &argv[argc+1];
	
	// kernel sets up apple pointer to be just past end of envp array
	const char** apple = envp;
	while(*apple != NULL) { ++apple; }
	++apple;

	// set up random value for stack canary
    
    // 栈溢出保护
	__guard_setup(apple);

#if DYLD_INITIALIZER_SUPPORT
	// run all C++ initializers inside dyld
	runDyldInitializers(argc, argv, envp, apple);
#endif

	// now that we are done bootstrapping dyld, call dyld's main
	uintptr_t appsSlide = appsMachHeader->getSlide();
    
    // 进入dyld::_main()函数
	return dyld::_main((macho_header*)appsMachHeader, appsSlide, argc, argv, envp, apple, startGlue);
}


#if TARGET_OS_SIMULATOR

extern "C" uintptr_t start_sim(int argc, const char* argv[], const char* envp[], const char* apple[],
							const dyld3::MachOLoaded* mainExecutableMH, const dyld3::MachOLoaded* dyldMH, uintptr_t dyldSlide,
							const dyld::SyscallHelpers*, uintptr_t* startGlue);
					

uintptr_t start_sim(int argc, const char* argv[], const char* envp[], const char* apple[],
					const dyld3::MachOLoaded* mainExecutableMH, const dyld3::MachOLoaded* dyldSimMH, uintptr_t dyldSlide,
					const dyld::SyscallHelpers* sc, uintptr_t* startGlue)
{
    // save table of syscall pointers
    gSyscallHelpers = sc;

	// dyld_sim uses chained rebases, so it always need to be fixed up
    rebaseDyld(dyldSimMH);

	// set up random value for stack canary
	__guard_setup(apple);

	// setup gProcessInfo to point to host dyld's struct
	dyld::gProcessInfo = (struct dyld_all_image_infos*)(sc->getProcessInfo());
	syncProcessInfo();

	// now that we are done bootstrapping dyld, call dyld's main
    uintptr_t appsSlide = mainExecutableMH->getSlide();
	return dyld::_main((macho_header*)mainExecutableMH, appsSlide, argc, argv, envp, apple, startGlue);
}
#endif


} // end of namespace



