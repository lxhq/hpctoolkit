// $Id$
// -*-C++-*-
// * BeginRiceCopyright *****************************************************
// 
// Copyright ((c)) 2002, Rice University 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
// 
// * Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
// 
// * Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
// 
// * Neither the name of Rice University (RICE) nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
// 
// This software is provided by RICE and contributors "as is" and any
// express or implied warranties, including, but not limited to, the
// implied warranties of merchantability and fitness for a particular
// purpose are disclaimed. In no event shall RICE or contributors be
// liable for any direct, indirect, incidental, special, exemplary, or
// consequential damages (including, but not limited to, procurement of
// substitute goods or services; loss of use, data, or profits; or
// business interruption) however caused and on any theory of liability,
// whether in contract, strict liability, or tort (including negligence
// or otherwise) arising in any way out of the use of this software, even
// if advised of the possibility of such damage. 
// 
// ******************************************************* EndRiceCopyright *

//***************************************************************************
//
// File:
//    Section.C
//
// Purpose:
//    [The purpose of this file]
//
// Description:
//    [The set of functions, macros, etc. defined in the file]
//
//***************************************************************************

//************************* System Include Files ****************************

//*************************** User Include Files ****************************

#include <include/gnu_bfd.h>

#include "Section.h"
#include "Procedure.h"
#include "Instruction.h"
#include <lib/support/Assertion.h>

#include <lib/ISA/ISA.h>

//*************************** Forward Declarations **************************

using std::cerr;
using std::endl;
using std::hex;
using std::dec;

//***************************************************************************

//***************************************************************************
// Section
//***************************************************************************

Section::Section(LoadModule* _lm, String _name, Type t,
		 Addr _start, Addr _end, Addr _sz)
  : lm(_lm), name(_name), type(t), start(_start), end(_end), size(_sz)
{
}

Section::~Section()
{
  lm = NULL; 
}

void
Section::Dump(std::ostream& o, const char* pre) const
{
  String p(pre);
  o << p << "------------------- Section Dump ------------------\n";
  o << p << "  Name: `" << GetName() << "'\n";
  o << p << "  Type: `";
  switch (GetType()) {
    case BSS:  o << "BSS'\n";  break;
    case Text: o << "Text'\n"; break;
    case Data: o << "Data'\n"; break;
    default:   o << "-unknown-'\n";  BriefAssertion(false); 
  }
  o << p << "  PC(start, end): 0x" << hex << GetStart() << ", 0x"
    << GetEnd() << dec << "\n";
  o << p << "  Size(b): " << GetSize() << "\n";
}

void
Section::DDump() const
{
  Dump(std::cerr);
}

//***************************************************************************
// TextSection
//***************************************************************************

class TextSectionImpl { 
public:
  asymbol **symTable; // we do not own
  char *contentsRaw; // allocated memory for section contents (we own)
  char *contents;    // contents, aligned with a 16-byte boundary
  int numSyms;
};

TextSection::TextSection(LoadModule* _lm, String _name, Addr _start, Addr _end,
			 suint _size, asymbol **syms, int numSyms, bfd *abfd)
  : Section(_lm, _name, Section::Text, _start, _end, _size), impl(NULL),
    procedures(0)
{
  impl = new TextSectionImpl;
  impl->symTable = syms; // we do not own 'syms'
  impl->numSyms = numSyms;
  impl->contents = NULL;

  // ------------------------------------------------------------
  // Each text section finds and creates its own routines.
  // Traverse the symbol table (which is sorted by VMA) searching
  // for function symbols in our section.  Create a Procedure for
  // each one found.
  // ------------------------------------------------------------
  for (int i = 0; i < impl->numSyms; i++) {
    // FIXME: exploit the fact that the symbol table is sorted by vma
    asymbol *sym = impl->symTable[i]; 
    if (IsIn(bfd_asymbol_value(sym)) && (sym->flags & BSF_FUNCTION)
        && !bfd_is_und_section(sym->section)) {
      Procedure::Type procType;

      if (sym->flags & BSF_LOCAL)
        procType = Procedure::Local;
      else if (sym->flags & BSF_WEAK)
        procType = Procedure::Weak;
      else if (sym->flags & BSF_GLOBAL)
        procType = Procedure::Global;
      else
        procType = Procedure::Unknown;
      
      Addr extent = FindProcedureExtent(i);
      String procNm = FindProcedureName(abfd, sym);
      String symNm  = bfd_asymbol_name(sym);
      Procedure *proc = new Procedure(this, procNm, symNm, procType,
		       /*start, end*/ bfd_asymbol_value(sym), extent,
			     /*size*/ extent - bfd_asymbol_value(sym));
      procedures.push_back(proc);
    } 
  }

  // ------------------------------------------------------------
  // Next, read in the section data (usually raw instructions).
  // ------------------------------------------------------------
  
  // Obtain a new buffer, and align the pointer to a 16-byte boundary.
  // We also add a 16 byte buffer at the beginning of the contents.
  //   This is because some of the GNU decoders (e.g. Sparc) want to
  //   examine both an instruction and its predecessor at the same time.
  //   Since we do not want to tell them about text section sizes -- the
  //   ISA classes are independent of these details -- we add this
  //   padding to prevent array access errors when decoding the first
  //   instruciton.
  
  // FIXME: Does "new" provide a way of returning an aligned pointer?
  impl->contentsRaw = new char[_size+16+16];
  memset(impl->contentsRaw, 0, 16+16);        // zero the padding
  char* contentsTmp = impl->contentsRaw + 16; // add the padding
  impl->contents = (char *)( ((bfd_vma)contentsTmp + 15) & ~15 ); // align

  const char *nameStr = (const char *)_name;
  int result = bfd_get_section_contents(abfd,
                                        bfd_get_section_by_name(abfd, nameStr),
                                        impl->contents, 0, _size);
  if (!result) {
    delete [] impl->contentsRaw;
    impl->contentsRaw = impl->contents = NULL;
    cerr << "Error reading section contents: " << bfd_errmsg(bfd_get_error())
	 << endl;
    return;
  }

  // ------------------------------------------------------------
  // Now disassemble the instructions in each procedure.
  // ------------------------------------------------------------
  Addr sectionBase = GetStart();
  
  for (TextSectionProcedureIterator it(*this); it.IsValid(); ++it) {
    Procedure* p = it.Current();
    Addr procStart = p->GetStartAddr();
    Addr procEnd = p->GetEndAddr();
    ushort instSz = 0;
    Addr lastInstPC = procStart; // pc of last valid instruction in the proc

    // Iterate over each pc at which an instruction might begin
    for (Addr pc = procStart; pc < procEnd; ) {
      MachInst *mi = &(impl->contents[pc - sectionBase]);
      instSz = isa->GetInstSize(mi);
      if (instSz == 0) {
	// This is not a recognized instruction (cf. data on CISC ISAs).
	++pc; // Increment the PC, and try to decode again.
	continue;
      }

      int num_ops = isa->GetInstNumOps(mi);
      if (num_ops == 0) {
	// This instruction contains data.  No need to decode.
	pc += instSz;
	continue;
      }

      // We have a valid instruction at this pc!
      lastInstPC = pc;
      for (ushort opIndex = 0; opIndex < num_ops; opIndex++) {
        Instruction *newInst = MakeInstruction(abfd, mi, pc, opIndex, instSz);
        _lm->AddInst(pc, opIndex, newInst); 
      }
      pc += instSz; 
    }
    // 'instSz' is now the size of the last instruction or 0

    // Now we can update the procedure's end address and size since we know
    // where the last instruction begins.  The procedure's original end
    // address was guessed to be the start address of the following procedure
    // while determining all procedures above.
    p->SetEndAddr(lastInstPC);
    p->SetSize(p->GetEndAddr() - p->GetStartAddr() + instSz); 
  }
}


TextSection::~TextSection()
{
  // Clear impl
  impl->symTable = NULL;
  delete[] impl->contentsRaw; impl->contentsRaw = NULL;
  impl->contents = NULL;
  delete impl;
    
  // Clear procedures
  for (TextSectionProcedureIterator it(*this); it.IsValid(); ++it) {
    delete it.Current(); // Procedure*
  }
  procedures.clear();
}

void
TextSection::Dump(std::ostream& o, const char* pre) const
{
  String p(pre);
  String p1 = p + "  ";

  Section::Dump(o, pre);
  o << p << "  Procedures (" << GetNumProcedures() << ")\n";
  for (TextSectionProcedureIterator it(*this); it.IsValid(); ++it) {
    Procedure* p = it.Current();
    p->Dump(o, p1);
  }
}

void
TextSection::DDump() const
{
  Dump(std::cerr);
}


//***************************   private members    ***************************

// Returns the name of the procedure referenced by 'procSym' using
// debugging information, if possible; otherwise returns the symbol
// name.
String
TextSection::FindProcedureName(bfd *abfd, asymbol *procSym) const
{
  String procName;
  const char* func = NULL, * file = NULL;
  unsigned int bfd_line = 0;

  // cf. LoadModule::GetSourceFileInfo
  asection *bfdSection = bfd_get_section_by_name(abfd, GetName());
  bfd_vma secBase = bfd_section_vma(abfd, bfdSection);
  bfd_vma symVal = bfd_asymbol_value(procSym);
  if (bfdSection) {
    bfd_find_nearest_line(abfd, bfdSection, impl->symTable,
			  symVal - secBase, &file, &func, &bfd_line);
  }

  if (func && (strlen(func) > 0)) {
    procName = func;
  } else {
    procName = bfd_asymbol_name(procSym); 
  }
  
  return procName;
}

// Find extent of the function given by funcSymIndex.  This is
// normally the address of the next function symbol in this section.
// However, if this is the last function in the section, then its
// extent is the address of the end of the section.
Addr
TextSection::FindProcedureExtent(int funcSymIndex) const
{
  // Since the symbol table we get is sorted by VMA, we can stop
  // the search as soon as we've gone beyond the VMA of this section.
  Addr ret = GetEnd();
  for (int next = funcSymIndex + 1; next < impl->numSyms; next++) {
    asymbol *sym = impl->symTable[next];
    if (!IsIn(bfd_asymbol_value(sym))) {
      break;
    }
    if ((sym->flags & BSF_FUNCTION) && !bfd_is_und_section(sym->section)) {
      ret = bfd_asymbol_value(sym); 
      break;
    } 
  }
  return ret;
}

// Returns a new instruction of the appropriate type.  Promises not to
// return NULL.
Instruction*
TextSection::MakeInstruction(bfd *abfd, MachInst* mi, Addr pc, ushort opIndex,
			     ushort sz) const
{
  // Assume that there is only one instruction type per
  // architecture (unlike i860 for example).
  Instruction *newInst = NULL;
  switch (bfd_get_arch(abfd)) {
    case bfd_arch_mips:
    case bfd_arch_alpha:
    case bfd_arch_sparc:
      newInst = new RISCInstruction(mi, pc);
      break;
    case bfd_arch_i386:
      newInst = new CISCInstruction(mi, pc, sz);
      break;
    case bfd_arch_ia64:
      newInst = new VLIWInstruction(mi, pc, opIndex);
      break;
    default:
      cerr << "Section.C: Could not create Instruction." << endl;
      BriefAssertion(false);
  }
  return newInst;
}

//***************************************************************************
// TextSectionProcedureIterator
//***************************************************************************

TextSectionProcedureIterator::TextSectionProcedureIterator(const TextSection& _sec)
  : sec(_sec)
{
  Reset();
}

TextSectionProcedureIterator::~TextSectionProcedureIterator()
{
}
