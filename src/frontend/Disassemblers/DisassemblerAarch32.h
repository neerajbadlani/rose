#ifndef ROSE_BinaryAnalysis_DisassemblerAarch32_H
#define ROSE_BinaryAnalysis_DisassemblerAarch32_H
#include <Disassembler.h>
#ifdef ROSE_ENABLE_ASM_AARCH32

#include <capstone/capstone.h>

namespace Rose {
namespace BinaryAnalysis {

/** Instruction decoder for ARM A32 and T32 instruction sets.
 *
 * Most of the useful disassembly methods can be found in the @ref Disassembler superclass.  Some of the constants
 * have the same ill-defined meanings as they do in the Capstone library. */
class DisassemblerAarch32: public Disassembler {
public:
    /** Capstone "Mode type", limited to those related to AArch32. Warning: these are non-orthogonal concepts. */
    enum Mode {
        MODE_ARM32 = CS_MODE_ARM,                       /**< Capstone: "32-bit ARM". */ // probably zero, not really a bit flag
        MODE_THUMB = CS_MODE_THUMB,                     /**< Capstone: "ARM's Thumb mode, including Thumb-2". */
        MODE_MCLASS = CS_MODE_MCLASS,                   /**< Capstone: "ARM's Cortex-M series". */
        MODE_V8 = CS_MODE_V8                            /**< Capstone: "ARMv8 A32 encodngs for ARM". */
    };

    /** Collection of Modes. */
    using Modes = BitFlags<Mode>;

private:
    Modes modes_;                                       // a subset of Capstone's cs_mode constants (warning: nonorthoganal concepts)
    csh capstone_;                                      // the capstone handle
    bool capstoneOpened_;                               // whether capstone_ is initialized

public:
    /** Constructor for specific architecture. */
    explicit DisassemblerAarch32(Modes modes = Modes())
        : modes_(modes), capstoneOpened_(false) {
        init();
    }

    ~DisassemblerAarch32();

    // overrides
    bool canDisassemble(SgAsmGenericHeader*) const override;
    Disassembler* clone() const override;
    Unparser::BasePtr unparser() const override;
    SgAsmInstruction* disassembleOne(const MemoryMap::Ptr&, rose_addr_t startVa, AddressSet *successors=NULL) override;
    SgAsmInstruction* makeUnknownInstruction(const Exception&) override;

private:
    void init();
};

} // namespace
} // namespace

#endif
#endif
