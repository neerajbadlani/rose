#include "sage3basic.h"
#include "CodeThornException.h"
#include "EquiThornCommandLineOptions.h"
#include "CppStdUtilities.h"

#include <string>
#include <sstream>
#include <iostream>

// required for checking of: HAVE_SPOT, HAVE_Z3
#include "rose_config.h"

#include "Diagnostics.h"
using namespace Sawyer::Message;
using namespace std;
using namespace CodeThorn;

CodeThorn::CommandLineOptions& parseCommandLine(int argc, char* argv[], Sawyer::Message::Facility logger, std::string version,
                                                CodeThornOptions& ctOpt, LTLOptions& ltlOpt, ParProOptions& parProOpt) {

  // Command line option handling.
  po::options_description visibleOptions("Supported options");
  po::options_description hiddenOptions("Hidden options");
  po::options_description passOnToRoseOptions("Options passed on to ROSE frontend");
  po::options_description equivalenceCheckingOptions("Equivalence checking options");
  po::options_description dataRaceOptions("Data race detection options");
  po::options_description experimentalOptions("Experimental options");
  po::options_description visualizationOptions("Visualization options");
  po::options_description infoOptions("Program information options");

  hiddenOptions.add_options()
    ("max-transitions-forced-top1",po::value< int >(&ctOpt.maxTransitionsForcedTop1)->default_value(-1),"Performs approximation after <arg> transitions (only exact for input,output).")
    ("max-transitions-forced-top2",po::value< int >(&ctOpt.maxTransitionsForcedTop2)->default_value(-1),"Performs approximation after <arg> transitions (only exact for input,output,df).")
    ("max-transitions-forced-top3",po::value< int >(&ctOpt.maxTransitionsForcedTop3)->default_value(-1),"Performs approximation after <arg> transitions (only exact for input,output,df,ptr-vars).")
    ("max-transitions-forced-top4",po::value< int >(&ctOpt.maxTransitionsForcedTop4)->default_value(-1),"Performs approximation after <arg> transitions (exact for all but inc-vars).")
    ("max-transitions-forced-top5",po::value< int >(&ctOpt.maxTransitionsForcedTop5)->default_value(-1),"Performs approximation after <arg> transitions (exact for input,output,df and vars with 0 to 2 assigned values)).")
    ("solver",po::value< int >(&ctOpt.solver)->default_value(5),"Set solver <arg> to use (one of 1,2,3,...).")
    ;

  passOnToRoseOptions.add_options()
    (",I", po::value< vector<string> >(&ctOpt.includeDirs),"Include directories.")
    (",D", po::value< vector<string> >(&ctOpt.preProcessorDefines),"Define constants for preprocessor.")
    ("edg:no_warnings", po::bool_switch(&ctOpt.edgNoWarningsFlag),"EDG frontend flag.")
    ("rose:ast:read", po::value<std::string>(&ctOpt.roseAstReadFileName),"read in binary AST from comma separated list (no spaces)")
    ;

  visualizationOptions.add_options()
    ("rw-clusters", po::value< bool >(&ctOpt.visualization.rwClusters)->default_value(false)->implicit_value(true), "Draw boxes around data elements from the same array (read/write-set graphs).")      
    ("rw-data", po::value< bool >(&ctOpt.visualization.rwData)->default_value(false)->implicit_value(true), "Display names of data elements (read/write-set graphs).") 
    ("rw-highlight-races", po::value< bool >(&ctOpt.visualization.rwHighlightRaces)->default_value(false)->implicit_value(true), "Highlight data races as large red dots (read/write-set graphs).") 
    ("dot-io-stg", po::value< string >(&ctOpt.visualization. dotIOStg), "Output STG with explicit I/O node information in dot file <arg>.")
    ("dot-io-stg-forced-top", po::value< string >(&ctOpt.visualization.dotIOStgForcedTop), "Output STG with explicit I/O node information in dot file <arg>. Groups abstract states together.")
    ("tg1-estate-address", po::value< bool >(&ctOpt.visualization.tg1EStateAddress)->default_value(false)->implicit_value(true), "Transition graph 1: Visualize address.")
    ("tg1-estate-id", po::value< bool >(&ctOpt.visualization.tg1EStateId)->default_value(true)->implicit_value(true), "Transition graph 1: Visualize estate-id.")
    ("tg1-estate-properties", po::value< bool >(&ctOpt.visualization.tg1EStateProperties)->default_value(true)->implicit_value(true), "Transition graph 1: Visualize all estate-properties.")
    ("tg1-estate-predicate", po::value< bool >(&ctOpt.visualization.tg1EStatePredicate)->default_value(false)->implicit_value(true), "Transition graph 1: Show estate as predicate.")
    ("tg1-estate-memory-subgraphs", po::value< bool >(&ctOpt.visualization.tg1EStateMemorySubgraphs)->default_value(false)->implicit_value(true), "Transition graph 1: Show estate as memory graphs.")
    ("tg2-estate-address", po::value< bool >(&ctOpt.visualization.tg2EStateAddress)->default_value(false)->implicit_value(true), "Transition graph 2: Visualize address.")
    ("tg2-estate-id", po::value< bool >(&ctOpt.visualization.tg2EStateId)->default_value(true)->implicit_value(true), "Transition graph 2: Visualize estate-id.")
    ("tg2-estate-properties", po::value< bool >(&ctOpt.visualization.tg2EStateProperties)->default_value(false)->implicit_value(true),"Transition graph 2: Visualize all estate-properties.")
    ("tg2-estate-predicate", po::value< bool >(&ctOpt.visualization.tg2EStatePredicate)->default_value(false)->implicit_value(true), "Transition graph 2: Show estate as predicate.")
    ("visualize-read-write-sets", po::value< bool >(&ctOpt.visualization.visualizeRWSets)->default_value(false)->implicit_value(true), "Generate a read/write-set graph that illustrates the read and write accesses of the involved threads.")
    ("viz", po::value< bool >(&ctOpt.visualization.viz)->default_value(false)->implicit_value(true),"Generate visualizations of AST, CFG, and transition system as dot files (ast.dot, cfg.dot, transitiongraph1/2.dot.")
    ("viz-tg2", po::value< bool >(&ctOpt.visualization.vizTg2)->default_value(false)->implicit_value(true),"Generate transition graph 2 (.dot).")
    ("cfg", po::value< string >(&ctOpt.visualization.icfgFileName), "Generate inter-procedural cfg as dot file. Each function is visualized as one dot cluster.")
    ;

  experimentalOptions.add_options()
    ("omp-ast", po::value< bool >(&ctOpt.ompAst)->default_value(false)->implicit_value(true),"Flag for using the OpenMP AST - useful when visualizing the ICFG.")
    ("normalize-all", po::value< bool >(&ctOpt.normalizeAll)->default_value(false)->implicit_value(true),"Normalize all expressions before analysis.")
    ("normalize-fcalls", po::value< bool >(&ctOpt.normalizeFCalls)->default_value(false)->implicit_value(true),"Normalize only expressions with function calls.")
    ("inline", po::value< bool >(&ctOpt.inlineFunctions)->default_value(false)->implicit_value(false),"inline functions before analysis .")
    ("inlinedepth",po::value< int >(&ctOpt.inlineFunctionsDepth)->default_value(10),"Default value is 10. A higher value inlines more levels of function calls.")
    ("eliminate-compound-assignments", po::value< bool >(&ctOpt.eliminateCompoundStatements)->default_value(true)->implicit_value(true),"Replace all compound-assignments by assignments.")
    ("stg-trace-file", po::value< string >(&ctOpt.stgTraceFileName), "Generate STG computation trace and write to file <arg>.")
    ("program-stats-only",po::value< bool >(&ctOpt.programStatsOnly)->default_value(false)->implicit_value(true),"print some basic program statistics about used language constructs and exit.")
    ("program-stats",po::value< bool >(&ctOpt.programStats)->default_value(false)->implicit_value(true),"print some basic program statistics about used language constructs.")
    ("in-state-string-literals",po::value< bool >(&ctOpt.inStateStringLiterals)->default_value(false)->implicit_value(true),"create string literals in initial state.")
    ("std-functions",po::value< bool >(&ctOpt.stdFunctions)->default_value(true)->implicit_value(true),"model std function semantics (malloc, memcpy, etc). Must be turned off explicitly.")
    ("ignore-function-pointers",po::value< bool >(&ctOpt.ignoreFunctionPointers)->default_value(false)->implicit_value(true), "Unknown functions are assumed to be side-effect free.")
    ("ignore-undefined-dereference",po::value< bool >(&ctOpt.ignoreUndefinedDereference)->default_value(false)->implicit_value(true), "Ignore pointer dereference of uninitalized value (assume data exists).")
    ("ignore-unknown-functions",po::value< bool >(&ctOpt.ignoreUnknownFunctions)->default_value(true)->implicit_value(true), "Ignore function pointers (functions are not called).")
    ("function-resolution-mode",po::value< int >(&ctOpt.functionResolutionMode)->default_value(4),"1:Translation unit only, 2:slow lookup, 3: -, 4: complete resolution (including function pointers)")
    ("context-sensitive",po::value< bool >(&ctOpt.contextSensitive)->default_value(true)->implicit_value(true),"Perform context sensitive analysis. Uses call strings with arbitrary length, recursion is not supported yet.")
    ("abstraction-mode",po::value< int >(&ctOpt.abstractionMode)->default_value(0),"Select abstraction mode (0: equality merge (explicit model checking), 1: approximating merge (abstract model checking).")
    ("interpreter-mode",po::value< int >(&ctOpt.interpreterMode)->default_value(0),"Select interpretation mode. 0: default, 1: execute stdout functions.")
    ("interpreter-mode-file",po::value< string >(&ctOpt.interpreterModeOuputFileName)->default_value(""),"Select interpretation mode output file (otherwise stdout is used).")
    ("print-warnings",po::value< bool >(&ctOpt.printWarnings)->default_value(false)->implicit_value(true),"Print warnings on stdout during analysis (this can slow down the analysis significantly)")
    ("print-violations",po::value< bool >(&ctOpt.printViolations)->default_value(false)->implicit_value(true),"Print detected violations on stdout during analysis (this can slow down the analysis significantly)")
    ("callstring-length",po::value< int >(&ctOpt.callStringLength)->default_value(10),"Set the length of the callstring for context-sensitive analysis. Default value is 10.")
    ;

  equivalenceCheckingOptions.add_options()
    ("dump-sorted",po::value< string >(&ctOpt.equiCheck.dumpSortedFileName), " (experimental) Generates sorted array updates in file <file>.")
    ("dump-non-sorted",po::value< string >(&ctOpt.equiCheck.dumpNonSortedFileName), " (experimental) Generates non-sorted array updates in file <file>.")
    ("rewrite-ssa", po::value< bool >(&ctOpt.equiCheck.rewriteSSA)->default_value(false)->implicit_value(true), "Rewrite SSA form: Replace use of SSA variable by rhs of its assignment (only applied outside loops or unrolled loops).")
    ("print-rewrite-trace", po::value< bool >(&ctOpt.equiCheck.printRewriteTrace)->default_value(false)->implicit_value(true), "Print trace of rewrite rules.")
    ("print-update-infos", po::value< bool >(&ctOpt.equiCheck.printUpdateInfos)->default_value(false)->implicit_value(true), "Print information about array updates on stdout.")
    ("rule-const-subst", po::value< bool >(&ctOpt.equiCheck.ruleConstSubst)->default_value(true)->implicit_value(true), "Use const-expr substitution rule.")
    ("rule-commutative-sort", po::value< bool >(&ctOpt.equiCheck.ruleCommutativeSort)->default_value(false)->implicit_value(true), "Apply rewrite rule for commutative sort of expression trees.")
    ("max-extracted-updates",po::value< int >(&ctOpt.equiCheck.maxExtractedUpdates)->default_value(5000)->implicit_value(-1),"Set maximum number of extracted updates. This ends the analysis.")
    ("specialize-fun-name", po::value< string >(&ctOpt.equiCheck.specializeFunName), "Function of name <arg> to be specialized.")
    ("specialize-fun-param", po::value< vector<int> >(&ctOpt.equiCheck.specializeFunParamList), "Function parameter number to be specialized (starting at 0).")
    ("specialize-fun-const", po::value< vector<int> >(&ctOpt.equiCheck.specializeFunConstList), "Constant <arg>, the param is to be specialized to.")
    ("specialize-fun-varinit", po::value< vector<string> >(&ctOpt.equiCheck.specializeFunVarInitList), "Variable name of which the initialization is to be specialized (overrides any initializer expression).")
    ("specialize-fun-varinit-const", po::value< vector<int> >(&ctOpt.equiCheck.specializeFunVarInitConstList), "Constant <arg>, the variable initialization is to be specialized to.")
    ;

  visibleOptions.add_options()            
    ("config,c", po::value< string >(&ctOpt.configFileName), "Use the configuration specified in file <arg>.")
    ("colors", po::value< bool >(&ctOpt.colors)->default_value(true)->implicit_value(true),"Use colors in output.")
    ("csv-stats",po::value< string >(&ctOpt.csvStatsFileName),"Output statistics into a CSV file <arg>.")
    ("display-diff",po::value< int >(&ctOpt.displayDiff)->default_value(-1),"Print statistics every <arg> computed estates.")
    ("exploration-mode",po::value< string >(&ctOpt.explorationMode), "Set mode in which state space is explored. ([breadth-first]|depth-first|loop-aware|loop-aware-sync)")
    ("quiet", po::value< bool >(&ctOpt.quiet)->default_value(false)->implicit_value(true), "Produce no output on screen.")
    ("help,h", "Produce this help message.")
    ("help-all", "Show all help options.")
    ("help-rose", "Show options that can be passed to ROSE.")
    ("help-eq", "Show options for program equivalence checking.")
    ("help-exp", "Show options for experimental features.")
    ("help-vis", "Show options for visualization output files.")
    ("help-info", "Show options for program info.")
    ("start-function", po::value< string >(&ctOpt.startFunctionName), "Name of function to start the analysis from.")
    ("external-function-calls-file",po::value< string >(&ctOpt.externalFunctionCallsFileName), "write a list of all function calls to external functions (functions for which no implementation exists) to a CSV file.")
    ("status", po::value< bool >(&ctOpt.status)->default_value(false)->implicit_value(true), "Show status messages.")
    ("reduce-cfg", po::value< bool >(&ctOpt.reduceCfg)->default_value(true)->implicit_value(true), "Reduce CFG nodes that are irrelevant for the analysis.")
    ("internal-checks", po::value< bool >(&ctOpt.internalChecks)->default_value(false)->implicit_value(true), "Run internal consistency checks (without input program).")
    ("cl-args",po::value< string >(&ctOpt.analyzedProgramCLArgs),"Specify command line options for the analyzed program (as one quoted string).")
    ("input-values",po::value< string >(&ctOpt.inputValues),"Specify a set of input values. (e.g. \"{1,2,3}\")")
    ("input-values-as-constraints", po::value< bool >(&ctOpt.inputValuesAsConstraints)->default_value(false)->implicit_value(true),"Represent input var values as constraints (otherwise as constants in PState).")
    ("input-sequence",po::value< string >(&ctOpt.inputSequence),"Specify a sequence of input values. (e.g. \"[1,2,3]\")")
    ("log-level",po::value< string >(&ctOpt.logLevel)->default_value("none,>=error"),"Set the log level (\"x,>=y\" with x,y in: (none|info|warn|trace|error|fatal|debug)).")
    ("max-transitions",po::value< int >(&ctOpt.maxTransitions)->default_value(-1),"Passes (possibly) incomplete STG to verifier after <arg> transitions have been computed.")
    ("max-iterations",po::value< int >(&ctOpt.maxIterations)->default_value(-1),"Passes (possibly) incomplete STG to verifier after <arg> loop iterations have been explored. Currently requires --exploration-mode=loop-aware[-sync].")
    ("max-time",po::value< long int >(&ctOpt.maxTime)->default_value(-1),"Stop computing the STG after an analysis time of approximately <arg> seconds has been reached.")
    ("max-transitions-forced-top",po::value< int >(&ctOpt.maxTransitionsForcedTop)->default_value(-1),"Performs approximation after <arg> transitions.")
    ("max-iterations-forced-top",po::value< int >(&ctOpt.maxIterationsForcedTop)->default_value(-1),"Performs approximation after <arg> loop iterations. Currently requires --exploration-mode=loop-aware[-sync].")
    ("max-time-forced-top",po::value< long int >(&ctOpt.maxTimeForcedTop)->default_value(-1),"Performs approximation after an analysis time of approximately <arg> seconds has been reached.")
    ("resource-limit-diff",po::value< int >(&ctOpt. resourceLimitDiff)->default_value(-1),"Check if the resource limit is reached every <arg> computed estates.")
    ("rewrite",po::value< bool >(&ctOpt.rewrite)->default_value(false)->implicit_value(true),"Rewrite AST applying all rewrite system rules.")
    ("run-rose-tests",po::value< bool >(&ctOpt.runRoseAstChecks)->default_value(false)->implicit_value(true), "Run ROSE AST tests.")
    ("analyzed-functions-csv",po::value<std::string>(&ctOpt.analyzedFunctionsCSVFileName),"Write list of analyzed functions to CSV file [arg].")
    ("analyzed-files-csv",po::value<std::string>(&ctOpt.analyzedFilesCSVFileName),"Write list of analyzed files (with analyzed functions) to CSV file [arg].")
    ("external-functions-csv",po::value<std::string>(&ctOpt.externalFunctionsCSVFileName),"Write list of external functions to CSV file [arg].")
    ("threads",po::value< int >(&ctOpt.threads)->default_value(1),"(experimental) Run analyzer in parallel using <arg> threads.")
    ("unparse",po::value< bool >(&ctOpt.unparse)->default_value(false)->implicit_value(true),"unpare code (only relevant for inlining, normalization, and lowering)")
    ("version,v",po::value< bool >(&ctOpt.displayVersion)->default_value(false)->implicit_value(true), "Display the version of CodeThorn.")
    ;

  infoOptions.add_options()
    ("print-variable-id-mapping",po::value< bool >(&ctOpt.info.printVariableIdMapping)->default_value(false)->implicit_value(true),"Print variable-id-mapping on stdout.")
    ("ast-stats-print",po::value< bool >(&ctOpt.info.printAstNodeStats)->default_value(false)->implicit_value(true),"Print ast node statistics on stdout.")
    ("ast-stats-csv",po::value< string >(&ctOpt.info.astNodeStatsCSVFileName),"Write ast node statistics to CSV file [arg].")
    ("type-size-mapping-print",po::value< bool >(&ctOpt.info.printTypeSizeMapping)->default_value(false)->implicit_value(true),"Print type-size mapping on stdout.")
    ("type-size-mapping-csv",po::value<std::string>(&ctOpt.info.typeSizeMappingCSVFileName),"Write type-size mapping to CSV file [arg].")
    ;

  po::options_description all("All supported options");
  all.add(visibleOptions)
    .add(hiddenOptions)
    .add(passOnToRoseOptions)
    .add(equivalenceCheckingOptions)
    .add(experimentalOptions)
    .add(visualizationOptions)
    .add(infoOptions)
    ;

  po::options_description configFileOptions("Configuration file options");
  configFileOptions.add(visibleOptions)
    .add(hiddenOptions)
    //    .add(passOnToRoseOptions) [cannot be used in config file]
    .add(equivalenceCheckingOptions)
    .add(experimentalOptions)
    .add(visualizationOptions)
    .add(infoOptions)
    ;

  args.parse(argc,argv,all,configFileOptions);

  if (args.isUserProvided("help")) {
    cout << visibleOptions << "\n";
    exit(0);
  } else if(args.isUserProvided("help-all")) {
    cout << all << "\n";
    exit(0);
  } else if(args.isUserProvided("help-eq")) {
    cout << equivalenceCheckingOptions << "\n";
    exit(0);
  } else if(args.isUserProvided("help-exp")) {
    cout << experimentalOptions << "\n";
    exit(0);
  } else if(args.isUserProvided("help-rose")) {
    cout << passOnToRoseOptions << "\n";
    exit(0);
  } else if(args.isUserProvided("help-vis")) {
    cout << visualizationOptions << "\n";
    exit(0);
  } else if(args.isUserProvided("help-info")) {
    cout << infoOptions << "\n";
    exit(0);
  } else if(ctOpt.displayVersion) {
    cout << "CodeThorn version "<<version<<endl;
    cout << "Written by Markus Schordan, Marc Jasper, Simon Schroder, Maximilan Fecke, Joshua Asplund, Adrian Prantl\n";
    exit(0);
  }

  // Additional checks for options passed on to the ROSE frontend.
  // "-std" is a short option with long name. Check that it still has an argument if used.
  // deactivated  // "-I" should either be followed by a whitespace or by a slash
  for (int i=1; i < argc; ++i) {
    string currentArg(argv[i]);
    if (currentArg == "-std") {
      logger[ERROR] << "Option \"-std\" requires an argument." << endl;
      ROSE_ASSERT(0);
    }
  }

  // Remove all CodeThorn-specific elements of argv (do not confuse ROSE frontend)
  for (int i=1; i < argc; ++i) {
    string currentArg(argv[i]);
    if (currentArg == "--rose:ast:read"){
      argv[i] = strdup("");
      ROSE_ASSERT(i+1<argc);
      argv[i+1]= strdup("");
      continue;
    }
    if (currentArg[0] != '-' ){
      continue;  // not an option      
    }
    // explicitly keep options relevant to the ROSE frontend (white list) 
    else if (currentArg == "-I") {
      assert(i+1<argc);
      ++i;
      continue;
    } else if (currentArg == "--edg:no_warnings") {
      continue;
    } else {
      string iPrefix = "-I/";
      string dPrefix = "-D"; // special case, cannot contain separating space
      if(currentArg.substr(0, iPrefix.size()) == iPrefix) {
        continue;
      }
      if(currentArg.substr(0, dPrefix.size()) == dPrefix) {
        continue;
      }
    }
    // No match with elements in the white list above. 
    // Must be a CodeThorn option, therefore remove it from argv.
    argv[i] = strdup("");
  }

  return args;
}

