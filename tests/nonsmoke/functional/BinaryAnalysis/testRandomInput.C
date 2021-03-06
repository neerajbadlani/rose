static const char *purpose = "tests pseudo-random inputs";
static const char *description =
    "This program tests that ROSE's compilers function reasonably when presented with random data, as might happen when "
    "the partitioner provisionally disassembles non-code in order to determine whether that region of memory is valid code. "
    "The disassemblers should not abort or produce output on standard error, although they may throw certain expected exceptions.";

#include <rose.h>

#include <BinaryUnparserBase.h>
#include <CommandLine.h>
#include <Diagnostics.h>
#include <Disassembler.h>
#include <LinearCongruentialGenerator.h>
#include <MemoryMap.h>
#include <Partitioner2/Engine.h>
#include <Partitioner2/Partitioner.h>
#include <Sawyer/AllocatingBuffer.h>
#include <Sawyer/CommandLine.h>
#include <SymbolicSemantics2.h>

using namespace Rose;
using namespace Rose::BinaryAnalysis;
using namespace Sawyer::Message::Common;
namespace P2 = Rose::BinaryAnalysis::Partitioner2;
namespace S2 = Rose::BinaryAnalysis::InstructionSemantics2;

Facility mlog;

struct Settings {
    size_t nBytes;                                      // number of bytes to disassemble
    int lcgSeed;                                        // seed value for the pseudo-random number generator
    std::string isa;                                    // instruction set architecture name
    bool showingInsns;

    Settings()
        : nBytes(1024), lcgSeed(0), isa("i386"), showingInsns(false) {}
};

void
parseCommandLine(int argc, char *argv[], Settings &settings) {
    using namespace Sawyer::CommandLine;
    Parser p = Rose::CommandLine::createEmptyParser(purpose, description);
    p.with(Rose::CommandLine::genericSwitches());

    SwitchGroup tool("Tool-specific switches");

    tool.insert(Switch("size", 'L')
                .argument("nbytes", nonNegativeIntegerParser(settings.nBytes))
                .doc("Number of bytes of random data to process as input."));

    tool.insert(Switch("seed")
                .argument("i", integerParser(settings.lcgSeed))
                .doc("Seed for initializing the pseudo-random number generator.  This can be used to generate repeatable "
                     "results."));

    tool.insert(Switch("isa")
                .argument("name", anyParser(settings.isa))
                .doc("Instruction set architecture name. This determines which disassembler is used. Specify \"list\" "
                     "to get a list of possible names."));

    CommandLine::insertBooleanSwitch(tool, "show-insn", settings.showingInsns,
                                     "Show disassembled instruction before running semantics.");

    if (!p.with(tool).parse(argc, argv).apply().unreachedArgs().empty()) {
        mlog[FATAL] <<"incorrect usage; see --help\n";
        exit(1);
    }
}

MemoryMap::Ptr
createInput(const Settings &settings) {
    // Generate input data
    uint8_t *tmp = new uint8_t[settings.nBytes];
    LinearCongruentialGenerator lcg(settings.lcgSeed);
    for (size_t i=0; i<settings.nBytes; ++i)
        tmp[i] = lcg();

    // Create the memory map by writing the input data into it
    MemoryMap::Ptr map = MemoryMap::instance();
    MemoryMap::Segment segment(MemoryMap::AllocatingBuffer::instance(settings.nBytes), 0, MemoryMap::READ_EXECUTE, "random data");
    map->insert(AddressInterval::baseSize(0, settings.nBytes), segment);
    map->at(0).limit(settings.nBytes).write(tmp);

    delete[] tmp;
    return map;
}

int
main(int argc, char *argv[]) {
    ROSE_INITIALIZE;
    Diagnostics::initAndRegister(&mlog, "tool");
    Settings settings;
    parseCommandLine(argc, argv, settings);
    MemoryMap::Ptr map = createInput(settings);

    // Obtain a disassembler
    try {
        if (!Disassembler::lookup(settings.isa))
            throw Disassembler::Exception("not found");
    } catch (const Disassembler::Exception&) {
        std::cerr <<"disassembler " <<settings.isa <<" is not supported in this configuration of ROSE\n";
        std::cerr <<"test is being skipped\n";
        return 0; // lack of a disassembler is not a test failure
    }
    P2::Engine engine;
    engine.settings().disassembler.isaName = settings.isa;
    P2::Partitioner partitioner = engine.createPartitioner();
    Disassembler *disassembler = partitioner.instructionProvider().disassembler();
    ASSERT_not_null(disassembler);

    // Configure unparser
    partitioner.insnUnparser()->settings().insn.bytes.showing = true;
#if 1 // FIXME: this is just here for A64 debugging
    partitioner.insnUnparser()->settings().insn.bytes.perLine = 4;
    partitioner.insnUnparser()->settings().insn.bytes.fieldWidth = 11;
#endif

    // Obtain an instruction semantics dispatcher if possible.
    S2::BaseSemantics::DispatcherPtr cpu = disassembler->dispatcher();
    if (cpu) {
        S2::BaseSemantics::RiscOperatorsPtr ops =
            S2::SymbolicSemantics::RiscOperators::instance(disassembler->registerDictionary());
        cpu = cpu->create(ops);
        mlog[INFO] <<"using symbolic semantics\n";
    } else {
        mlog[WARN] <<"instruction semantics aren't implemented for " <<disassembler->name() <<"\n";
    }
    
    // Disassemble each instruction. Since some architectures require instructions to be aligned, we start the next instruction
    // after the end of the current instruction. If an instruction cannot be disassembled at a particular address, then we try
    // at the next address even though that might not be aligned. In other words, on an architecture where alignment is
    // required, failure to disassemble a 4-byte instruction will most likely result in the next three addresses also failing
    // because of invalid alignment.
    mlog[INFO] <<"disassembling " <<StringUtility::plural(settings.nBytes, "bytes")
               <<" of random data (seed=" <<settings.lcgSeed <<")"
               <<" using the " <<disassembler->name() <<" disassembler\n";
    size_t nNulls=0, nUnknown=0, nGood=0, nDisExceptions=0, nSemExceptions=0;
    rose_addr_t va = 0;
    while (map->atOrAfter(va).next().assignTo(va)) {
        // Disassemble at current address
        SAWYER_MESG(mlog[DEBUG]) <<"disassembling at " <<StringUtility::addrToString(va) <<"\n";
        SgAsmInstruction *insn = NULL;
        try {
            insn = disassembler->disassembleOne(map, va);
            if (NULL == insn) {
                ++nNulls;
            } else if (insn->isUnknown()) {
                ++nUnknown;
            } else {
                ++nGood;
            }
        } catch (const Disassembler::Exception&) {
            ++nDisExceptions;
        }

        if (insn && settings.showingInsns)
            std::cerr <<"                            " <<partitioner.unparse(insn) <<"\n";

        // Run semantics. We use a fresh input state each time, otherwise the state would eventually get too large.
        if (cpu && insn && !insn->isUnknown()) {
            cpu->currentState()->clear();
            try {
                cpu->processInstruction(insn);
            } catch (const S2::BaseSemantics::Exception&) {
                ++nSemExceptions;
            }
        }

        // Increment to next address
        if (va == map->greatest())
            break;                                      // avoid possible overflow
        if (insn) {
            va += insn->get_size();
        } else {
            ++va;
        }
    }

    std::cout <<"good instructions:      " <<nGood <<"\n";
    std::cout <<"null instructions:      " <<nNulls <<"\n";
    std::cout <<"unknown instructions:   " <<nUnknown <<"\n";
    std::cout <<"disassembly exceptions: " <<nDisExceptions <<"\n";
    std::cout <<"semantic exceptions:    " <<nSemExceptions <<"\n";
}
