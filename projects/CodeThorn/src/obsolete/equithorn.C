/*************************************************************
 * Author   : Markus Schordan                                *
 *************************************************************/

#include "rose.h"

//#include "rose_config.h"

#include "codethorn.h"
#include "SgNodeHelper.h"
#include "Labeler.h"
#include "VariableIdMapping.h"
#include "EState.h"
#include "TimeMeasurement.h"
#include <cstdio>
#include <cstring>
#include <map>

#include "EquiThornCommandLineOptions.h"

#include "InternalChecks.h"
#include "AstAnnotator.h"
#include "AstTerm.h"
#include "AbstractValue.h"
#include "AstMatching.h"
#include "RewriteSystem.h"
#include "ltlthorn-lib/SpotConnection.h"
#include "ltlthorn-lib/CounterexampleAnalyzer.h"
#include "AstUtility.h"
#include "ArrayElementAccessData.h"
#include "PragmaHandler.h"
#include "Miscellaneous2.h"
#include "FIConstAnalysis.h"
#include "ReachabilityAnalysis.h"
#include "EquivalenceChecking.h"
#include "Solver5.h"
#include "Solver8.h"
#include "ltlthorn-lib/Solver10.h"
#include "ltlthorn-lib/Solver11.h"
#include "ltlthorn-lib/Solver12.h"
#include "ReadWriteAnalyzer.h"
#include "AnalysisParameters.h"
#include "CodeThornException.h"
#include "CodeThornException.h"
#include "ProgramInfo.h"
#include "FunctionCallMapping.h"
#include "AstStatistics.h"

#include "DataRaceDetection.h"
#include "AstTermRepresentation.h"
#include "Normalization.h"
#include "DataDependenceVisualizer.h" // also used for clustered ICFG
#include "Evaluator.h" // CppConstExprEvaluator

// Z3-based analyser / SSA 
#include "z3-prover-connection/SSAGenerator.h"
#include "z3-prover-connection/ReachabilityAnalyzerZ3.h"

// ParProAutomata
#include "ltlthorn-lib/ParProAutomata.h"

#if defined(__unix__) || defined(__unix) || defined(unix)
#include <sys/resource.h>
#endif

#include "CodeThornLib.h"
#include "LTLThornLib.h"
#include "CppStdUtilities.h"

//BOOST includes
#include "boost/lexical_cast.hpp"

using namespace std;

using namespace CodeThorn;
using namespace boost;

#include "Diagnostics.h"
using namespace Sawyer::Message;

// experimental
#include "IOSequenceGenerator.C"

#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

const std::string versionString="1.12.6";

void analyzerSetup(IOAnalyzer* analyzer, Sawyer::Message::Facility logger,
                   CodeThornOptions& ctOpt, LTLOptions& ltlOpt, ParProOptions& parProOpt) {
  analyzer->setOptionOutputWarnings(ctOpt.printWarnings);
  analyzer->setPrintDetectedViolations(ctOpt.printViolations);

  if(ctOpt.stgTraceFileName.size()>0) {
    analyzer->setStgTraceFileName(ctOpt.stgTraceFileName);
  }

  if(ctOpt.analyzedProgramCLArgs.size()>0) {
    string clOptions=ctOpt.analyzedProgramCLArgs;
    vector<string> clOptionsVector=Parse::commandLineArgs(clOptions);
    analyzer->setCommandLineOptions(clOptionsVector);
  }

  if(ctOpt.inputValues.size()>0) {
    cout << "STATUS: input-values="<<ctOpt.inputValues<<endl;
    set<int> intSet=Parse::integerSet(ctOpt.inputValues);
    for(set<int>::iterator i=intSet.begin();i!=intSet.end();++i) {
      analyzer->insertInputVarValue(*i);
    }
    cout << "STATUS: input-values stored."<<endl;
  }

  if(ctOpt.inputSequence.size()>0) {
    cout << "STATUS: input-sequence="<<ctOpt.inputSequence<<endl;
    list<int> intList=Parse::integerList(ctOpt.inputSequence);
    for(list<int>::iterator i=intList.begin();i!=intList.end();++i) {
      analyzer->addInputSequenceValue(*i);
    }
  }

  if(ctOpt.explorationMode.size()>0) {
    string explorationMode=ctOpt.explorationMode;
    if(explorationMode=="depth-first") {
      analyzer->setExplorationMode(EXPL_DEPTH_FIRST);
    } else if(explorationMode=="breadth-first") {
      analyzer->setExplorationMode(EXPL_BREADTH_FIRST);
    } else if(explorationMode=="loop-aware") {
      analyzer->setExplorationMode(EXPL_LOOP_AWARE);
    } else if(explorationMode=="loop-aware-sync") {
      analyzer->setExplorationMode(EXPL_LOOP_AWARE_SYNC);
    } else if(explorationMode=="random-mode1") {
      analyzer->setExplorationMode(EXPL_RANDOM_MODE1);
    } else {
      logger[ERROR] <<"unknown state space exploration mode specified with option --exploration-mode."<<endl;
      exit(1);
    }
  } else {
    // default value
    analyzer->setExplorationMode(EXPL_BREADTH_FIRST);
  }

  if (ctOpt.maxIterations!=-1 || ctOpt.maxIterationsForcedTop!=-1) {
    if(ctOpt.explorationMode!="loop-aware" && ctOpt.explorationMode!="loop-aware-sync") {
      cout << "Error: \"max-iterations[-forced-top]\" modes currently require \"--exploration-mode=loop-aware[-sync]\"." << endl;
      exit(1);
    }
  }

  analyzer->setAbstractionMode(ctOpt.abstractionMode);
  analyzer->setMaxTransitions(ctOpt.maxTransitions);
  analyzer->setMaxIterations(ctOpt.maxIterations);

  if(ctOpt.maxIterationsForcedTop!=-1) {
    analyzer->setMaxIterationsForcedTop(ctOpt.maxIterationsForcedTop);
    analyzer->setGlobalTopifyMode(CTAnalysis::GTM_IO);
  }

  // TODO: CTAnalysis::GTM_IO is only mode used now, all others are deprecated
  if(ctOpt.maxTransitionsForcedTop!=-1) {
    analyzer->setMaxTransitionsForcedTop(ctOpt.maxTransitionsForcedTop);
    analyzer->setGlobalTopifyMode(CTAnalysis::GTM_IO);
  } else if(ctOpt.maxTransitionsForcedTop1!=-1) {
    analyzer->setMaxTransitionsForcedTop(ctOpt.maxTransitionsForcedTop1);
    analyzer->setGlobalTopifyMode(CTAnalysis::GTM_IO);
  } else if(ctOpt.maxTransitionsForcedTop2!=-1) {
    analyzer->setMaxTransitionsForcedTop(ctOpt.maxTransitionsForcedTop2);
    analyzer->setGlobalTopifyMode(CTAnalysis::GTM_IOCF);
  } else if(ctOpt.maxTransitionsForcedTop3!=-1) {
    analyzer->setMaxTransitionsForcedTop(ctOpt.maxTransitionsForcedTop3);
    analyzer->setGlobalTopifyMode(CTAnalysis::GTM_IOCFPTR);
  } else if(ctOpt.maxTransitionsForcedTop4!=-1) {
    analyzer->setMaxTransitionsForcedTop(ctOpt.maxTransitionsForcedTop4);
    analyzer->setGlobalTopifyMode(CTAnalysis::GTM_COMPOUNDASSIGN);
  } else if(ctOpt.maxTransitionsForcedTop5!=-1) {
    analyzer->setMaxTransitionsForcedTop(ctOpt.maxTransitionsForcedTop5);
    analyzer->setGlobalTopifyMode(CTAnalysis::GTM_FLAGS);
  }

  if(ctOpt.displayDiff!=-1) {
    analyzer->setDisplayDiff(ctOpt.displayDiff);
  }
  if(ctOpt.resourceLimitDiff!=-1) {
    analyzer->setResourceLimitDiff(ctOpt.resourceLimitDiff);
  }

  analyzer->setSolver(new Solver5());
}

int main( int argc, char * argv[] ) {
  try {
    ROSE_INITIALIZE;
    CodeThorn::configureRose();

    TimeMeasurement timer;
    timer.start();

    CodeThornOptions ctOpt;
    LTLOptions ltlOpt; // to be moved into separate tool
    ParProOptions parProOpt; // to be moved into separate tool
    parseCommandLine(argc, argv, logger,versionString,ctOpt,ltlOpt,parProOpt);

    // Start execution
    mfacilities.control(ctOpt.logLevel);
    SAWYER_MESG(logger[TRACE]) << "Log level is " << ctOpt.logLevel << endl;

    // ParPro command line options
    bool exitRequest=CodeThorn::ParProAutomata::handleCommandLineArguments(parProOpt,ctOpt,ltlOpt,logger);
    if(exitRequest) {
      exit(0);
    }

    IOAnalyzer* analyzer;
    analyzer = new IOAnalyzer();
    analyzer->setOptions(ctOpt);
    analyzer->setLtlOptions(ltlOpt);

    if(ctOpt.internalChecks) {
      mfacilities.shutdown();
      if(CodeThorn::internalChecks(argc,argv)==false)
        return 1;
      else
        return 0;
    }

    analyzer->optionStringLiteralsInState=ctOpt.inStateStringLiterals;
    analyzer->setSkipUnknownFunctionCalls(ctOpt.ignoreUnknownFunctions);
    analyzer->setIgnoreFunctionPointers(ctOpt.ignoreFunctionPointers);
    analyzer->setStdFunctionSemantics(ctOpt.stdFunctions);

    analyzerSetup(analyzer, logger, ctOpt, ltlOpt, parProOpt);

    switch(int mode=ctOpt.interpreterMode) {
    case 0: analyzer->setInterpreterMode(IM_DISABLED); break;
    case 1: analyzer->setInterpreterMode(IM_ENABLED); break;
    default:
      cerr<<"Unknown interpreter mode "<<mode<<" provided on command line (supported: 0..1)."<<endl;
      exit(1);
    }
    string outFileName=ctOpt.interpreterModeOuputFileName;
    if(outFileName!="") {
      analyzer->setInterpreterModeOutputFileName(outFileName);
      CppStdUtilities::writeFile(outFileName,""); // touch file
    }
    {
      switch(int argVal=ctOpt.functionResolutionMode) {
      case 1: CFAnalysis::functionResolutionMode=CFAnalysis::FRM_TRANSLATION_UNIT;break;
      case 2: CFAnalysis::functionResolutionMode=CFAnalysis::FRM_WHOLE_AST_LOOKUP;break;
      case 4: CFAnalysis::functionResolutionMode=CFAnalysis::FRM_FUNCTION_CALL_MAPPING;break;
      default: 
        cerr<<"Error: unsupported argument value of "<<argVal<<" for function-resolution-mode.";
        exit(1);
      }
    }
    // analyzer->setFunctionResolutionMode(ctOpt.functionResolutionMode);
    // needs to set CFAnalysis functionResolutionMode

    int numThreads=ctOpt.threads; // default is 1
    if(numThreads<=0) {
      cerr<<"Error: number of threads must be greater or equal 1."<<endl;
      exit(1);
    }
    analyzer->setNumberOfThreadsToUse(numThreads);

    string option_start_function="main";
    if(ctOpt.startFunctionName.size()>0) {
      option_start_function = ctOpt.startFunctionName;
    }

    string option_specialize_fun_name="";
    vector<int> option_specialize_fun_param_list;
    vector<int> option_specialize_fun_const_list;
    vector<string> option_specialize_fun_varinit_list;
    vector<int> option_specialize_fun_varinit_const_list;
    if(ctOpt.equiCheck.specializeFunName.size()>0) {
      option_specialize_fun_name = ctOpt.equiCheck.specializeFunName;
      // logger[DEBUG] << "option_specialize_fun_name: "<< option_specialize_fun_name<<endl;
    } else {
      // logger[DEBUG] << "option_specialize_fun_name: NONE"<< option_specialize_fun_name<<endl;
    }

    if(ctOpt.equiCheck.specializeFunParamList.size()>0) {
      option_specialize_fun_param_list=ctOpt.equiCheck.specializeFunParamList;
      option_specialize_fun_const_list=ctOpt.equiCheck.specializeFunConstList;
    }

    if(ctOpt.equiCheck.specializeFunVarInitList.size()>0) {
      option_specialize_fun_varinit_list=ctOpt.equiCheck.specializeFunVarInitList;
      option_specialize_fun_varinit_const_list=ctOpt.equiCheck.specializeFunVarInitConstList;
    }

    // logger[DEBUG] << "specialize-params:"<<option_specialize_fun_const_list.size()<<endl;

    if(ctOpt.equiCheck.specializeFunName.size()>0) {
      if( ((ctOpt.equiCheck.specializeFunParamList.size()>0)||ctOpt.equiCheck.specializeFunConstList.size()>0)
        && !(ctOpt.equiCheck.specializeFunName.size()>0&&ctOpt.equiCheck.specializeFunParamList.size()>0)) {
        logger[ERROR] <<"options --specialize-fun-name=NAME --specialize-fun-param=NUM --specialize-fun-const=NUM must be used together."<<endl;
        exit(1);
      }
    if((ctOpt.equiCheck.specializeFunVarInitList.size()>0||ctOpt.equiCheck.specializeFunVarInitConstList.size()>0)
       && !(ctOpt.equiCheck.specializeFunVarInitList.size()>0&&ctOpt.equiCheck.specializeFunVarInitConstList.size()>0)) {
        logger[ERROR] <<"options --specialize-fun-name=NAME --specialize-fun-varinit=NAME --specialize-fun-const=NUM must be used together."<<endl;
        exit(1);
      }
    }

    if((ctOpt.equiCheck.printUpdateInfos)&&(ctOpt.equiCheck.dumpSortedFileName.size()==0 && ctOpt.equiCheck.dumpNonSortedFileName.size()==0)) {
      logger[ERROR] <<"option print-update-infos/equivalence-check must be used together with option --dump-non-sorted or --dump-sorted."<<endl;
      exit(1);
    }
    RewriteSystem rewriteSystem;
    if(ctOpt.equiCheck.printRewriteTrace) {
      rewriteSystem.setTrace(true);
    }
    if(ctOpt.ignoreUndefinedDereference) {
      analyzer->setIgnoreUndefinedDereference(true);
    }
    if(ctOpt.equiCheck.dumpSortedFileName.size()>0 || ctOpt.equiCheck.dumpNonSortedFileName.size()>0) {
      analyzer->setSkipUnknownFunctionCalls(true);
      analyzer->setSkipArrayAccesses(true);
      if(analyzer->getNumberOfThreadsToUse()>1) {
        logger[ERROR] << "multi threaded rewrite not supported yet."<<endl;
        exit(1);
      }
    }

    // Build the AST used by ROSE
    if(ctOpt.status) {
      cout<< "STATUS: Parsing and creating AST started."<<endl;
    }

    SgProject* sageProject = 0;
    vector<string> argvList(argv,argv+argc);
    //string turnOffRoseLoggerWarnings="-rose:log none";
    //    argvList.push_back(turnOffRoseLoggerWarnings);
    if(ctOpt.ompAst||ctOpt.dr.detection) {
      SAWYER_MESG(logger[TRACE])<<"selected OpenMP AST."<<endl;
      argvList.push_back("-rose:OpenMP:ast_only");
    }
    if(ctOpt.roseAstReadFileName.size()>0) {
      // add ROSE option as required non-standard single dash long option
      argvList.push_back("-rose:ast:read");
      argvList.push_back(ctOpt.roseAstReadFileName);
    }
    timer.stop();

    timer.start();
    sageProject=frontend(argvList);
    double frontEndRunTime=timer.getTimeDurationAndStop().milliSeconds();

    if(ctOpt.status) {
      cout << "STATUS: Parsing and creating AST finished."<<endl;
    }

    /* perform inlining before variable ids are computed, because
       variables are duplicated by inlining. */
    timer.start();
    double normalizationRunTime=timer.getTimeDurationAndStop().milliSeconds();

    /* Context sensitive analysis using call strings.
     */
    {
      analyzer->setOptionContextSensitiveAnalysis(ctOpt.contextSensitive);
      //Call strings length abrivation is not supported yet.
      //CodeThorn::CallString::setMaxLength(_ctOpt.callStringLength);
    }

    {
      bool unknownFunctionsFile=ctOpt.externalFunctionsCSVFileName.size()>0;
      bool showProgramStats=ctOpt.programStats;
      bool showProgramStatsOnly=ctOpt.programStatsOnly;
      if(unknownFunctionsFile||showProgramStats||showProgramStatsOnly) {
        ProgramInfo programInfo(sageProject);
        programInfo.compute();
        if(unknownFunctionsFile) {
          ROSE_ASSERT(analyzer);
          programInfo.writeFunctionCallNodesToFile(ctOpt.externalFunctionsCSVFileName);
        }
        if(showProgramStats||showProgramStatsOnly) {
          programInfo.printDetailed();
        }
        if(showProgramStatsOnly) {
          exit(0);
        }
      }
    }

    if(ctOpt.unparse) {
      sageProject->unparse(0,0);
      return 0;
    }

    if(ctOpt.status) {
      cout<<"STATUS: analysis started."<<endl;
    }

    SgNode* root=sageProject;
    ROSE_ASSERT(root);

    // only handle pragmas if fun_name is not set on the command line
    if(option_specialize_fun_name=="") {
      SAWYER_MESG(logger[TRACE])<<"STATUS: handling pragmas started."<<endl;
      PragmaHandler pragmaHandler;
      pragmaHandler.handlePragmas(sageProject,analyzer);
      // TODO: requires more refactoring
      option_specialize_fun_name=pragmaHandler.option_specialize_fun_name;
      // unparse specialized code
      //sageProject->unparse(0,0);
      SAWYER_MESG(logger[TRACE])<<"STATUS: handling pragmas finished."<<endl;
    } else {
      // do specialization and setup data structures
      analyzer->setSkipUnknownFunctionCalls(true);
      analyzer->setSkipArrayAccesses(true);
      analyzer->setOptions(ctOpt);
      //TODO1: refactor into separate function
      int numSubst=0;
      if(option_specialize_fun_name!="") {
        Specialization speci;
        SAWYER_MESG(logger[TRACE])<<"STATUS: specializing function: "<<option_specialize_fun_name<<endl;

        string funNameToFind=option_specialize_fun_name;

        for(size_t i=0;i<option_specialize_fun_param_list.size();i++) {
          int param=option_specialize_fun_param_list[i];
          int constInt=option_specialize_fun_const_list[i];
          numSubst+=speci.specializeFunction(sageProject,funNameToFind, param, constInt, analyzer->getVariableIdMapping());
        }
        SAWYER_MESG(logger[TRACE])<<"STATUS: specialization: number of variable-uses replaced with constant: "<<numSubst<<endl;
        int numInit=0;
        logger[INFO]<<"var init spec: "<<endl;
        for(size_t i=0;i<option_specialize_fun_varinit_list.size();i++) {
          string varInit=option_specialize_fun_varinit_list[i];
          int varInitConstInt=option_specialize_fun_varinit_const_list[i];
          logger[INFO]<<"checking for varInitName nr "<<i<<" var:"<<varInit<<" Const:"<<varInitConstInt<<endl;
          numInit+=speci.specializeFunction(sageProject,funNameToFind, -1, 0, varInit, varInitConstInt,analyzer->getVariableIdMapping());
        }
        SAWYER_MESG(logger[TRACE])<<"STATUS: specialization: number of variable-inits replaced with constant: "<<numInit<<endl;
      }
    }

    if(ctOpt.rewrite) {
      SAWYER_MESG(logger[TRACE])<<"STATUS: rewrite started."<<endl;
      rewriteSystem.resetStatistics();
      rewriteSystem.setRewriteCondStmt(false); // experimental: supposed to normalize conditions
      rewriteSystem.rewriteAst(root,analyzer->getVariableIdMapping(), false, true/*eliminate compound assignments*/);
      // TODO: Outputs statistics
      cout <<"Rewrite statistics:"<<endl<<rewriteSystem.getStatistics().toString()<<endl;
      sageProject->unparse(0,0);
      SAWYER_MESG(logger[TRACE])<<"STATUS: generated rewritten program."<<endl;
      exit(0);
    }

    /*
      deactivated. Requires variableIdMapping to be available, but it is not created yet
    if(ctOpt.eliminateCompoundStatements) {
      SAWYER_MESG(logger[TRACE])<<"STATUS: Elimination of compound assignments started."<<endl;
      set<AbstractValue> compoundIncVarsSet=AstUtility::determineSetOfCompoundIncVars(analyzer->getVariableIdMapping(),root);
      rewriteSystem.resetStatistics();
      rewriteSystem.rewriteCompoundAssignmentsInAst(root,analyzer->getVariableIdMapping());
      SAWYER_MESG(logger[TRACE])<<"STATUS: Elimination of compound assignments finished."<<endl;
    }
    */
    
    timer.start();
#if 0
    if(!analyzer->getVariableIdMapping()->isUniqueVariableSymbolMapping()) {
      logger[WARN] << "Variable<->Symbol mapping not bijective."<<endl;
      //varIdMap.reportUniqueVariableSymbolMappingViolations();
    }
#endif
#if 0
    analyzer->getVariableIdMapping()->toStream(cout);
#endif

    SAWYER_MESG(logger[TRACE])<< "INIT: creating solver "<<analyzer->getSolver()->getId()<<"."<<endl;

    if(option_specialize_fun_name!="") {
      analyzer->initializeSolver3(option_specialize_fun_name,sageProject,true);
    } else {
      // if main function exists, start with main-function
      // if a single function exist, use this function
      // in all other cases exit with error.
      RoseAst completeAst(root);
      string startFunction=option_start_function;
      SgNode* _startFunRoot=completeAst.findFunctionByName(startFunction);
      if(_startFunRoot==0) {
        // no main function exists. check if a single function exists in the translation unit
        SgProject* project=isSgProject(root);
        ROSE_ASSERT(project);
        std::list<SgFunctionDefinition*> funDefs=SgNodeHelper::listOfFunctionDefinitions(project);
        if(funDefs.size()==1) {
          // found exactly one function. Analyse this function.
          SgFunctionDefinition* functionDef=*funDefs.begin();
          startFunction=SgNodeHelper::getFunctionName(functionDef);
        } else if(funDefs.size()>1) {
          cerr<<"Error: no main function and more than one function in translation unit."<<endl;
          exit(1);
        } else if(funDefs.size()==0) {
          cerr<<"Error: no function in translation unit."<<endl;
          exit(1);
        }
      }
      ROSE_ASSERT(startFunction!="");
      analyzer->initializeSolver3(startFunction,sageProject,false);
    }
    analyzer->initLabeledAssertNodes(sageProject);

    // pattern search: requires that exploration mode is set,
    // otherwise no pattern search is performed
    if(ctOpt.patSearch.explorationMode.size()>0) {
      logger[INFO] << "Pattern search exploration mode was set. Choosing solver 10." << endl;
      analyzer->setSolver(new Solver10());
      analyzer->setStartPState(*analyzer->popWorkList()->pstate());
    }
    double initRunTime=timer.getTimeDurationAndStop().milliSeconds();
    
    timer.start();
    analyzer->printStatusMessageLine("==============================================================");
    if(!analyzer->getModeLTLDriven() && ctOpt.z3BasedReachabilityAnalysis==false && ctOpt.ssa==false) {
      analyzer->runSolver();
    }
    double analysisRunTime=timer.getTimeDurationAndStop().milliSeconds();

    analyzer->printStatusMessageLine("==============================================================");

    long pstateSetSize=analyzer->getPStateSet()->size();
    long pstateSetBytes=analyzer->getPStateSet()->memorySize();
    long pstateSetMaxCollisions=analyzer->getPStateSet()->maxCollisions();
    long pstateSetLoadFactor=analyzer->getPStateSet()->loadFactor();
    long eStateSetSize=analyzer->getEStateSet()->size();
    long eStateSetBytes=analyzer->getEStateSet()->memorySize();
    long eStateSetMaxCollisions=analyzer->getEStateSet()->maxCollisions();
    double eStateSetLoadFactor=analyzer->getEStateSet()->loadFactor();
    long transitionGraphSize=analyzer->getTransitionGraph()->size();
    long transitionGraphBytes=transitionGraphSize*sizeof(Transition);
    long numOfconstraintSets=analyzer->getConstraintSetMaintainer()->numberOf();
    long constraintSetsBytes=analyzer->getConstraintSetMaintainer()->memorySize();
    long constraintSetsMaxCollisions=analyzer->getConstraintSetMaintainer()->maxCollisions();
    double constraintSetsLoadFactor=analyzer->getConstraintSetMaintainer()->loadFactor();
    long numOfStdinEStates=(analyzer->getEStateSet()->numberOfIoTypeEStates(InputOutput::STDIN_VAR));
    long numOfStdoutVarEStates=(analyzer->getEStateSet()->numberOfIoTypeEStates(InputOutput::STDOUT_VAR));
    long numOfStdoutConstEStates=(analyzer->getEStateSet()->numberOfIoTypeEStates(InputOutput::STDOUT_CONST));
    long numOfStderrEStates=(analyzer->getEStateSet()->numberOfIoTypeEStates(InputOutput::STDERR_VAR));
    long numOfFailedAssertEStates=(analyzer->getEStateSet()->numberOfIoTypeEStates(InputOutput::FAILED_ASSERT));
    long numOfConstEStates=0;//(analyzer->getEStateSet()->numberOfConstEStates(analyzer->getVariableIdMapping()));
    long numOfStdoutEStates=numOfStdoutVarEStates+numOfStdoutConstEStates;

#if defined(__unix__) || defined(__unix) || defined(unix)
    // Unix-specific solution to finding the peak phyisical memory consumption (rss).
    // Not necessarily supported by every OS.
    struct rusage resourceUsage;
    getrusage(RUSAGE_SELF, &resourceUsage);
    long totalMemory=resourceUsage.ru_maxrss * 1024;
#else
    long totalMemory=pstateSetBytes+eStateSetBytes+transitionGraphBytes+constraintSetsBytes;
#endif

    double totalRunTime=frontEndRunTime+initRunTime+analysisRunTime;

    long pstateSetSizeInf = 0;
    long eStateSetSizeInf = 0;
    long transitionGraphSizeInf = 0;
    long eStateSetSizeStgInf = 0;
    double infPathsOnlyTime = 0;
    double stdIoOnlyTime = 0;

 
    long eStateSetSizeIoOnly = 0;
    long transitionGraphSizeIoOnly = 0;
    double spotLtlAnalysisTime = 0;

    stringstream statisticsSizeAndLtl;
    stringstream statisticsCegpra;

    double totalLtlRunTime =  0.0;

    // TEST
    if (ctOpt.generateAssertions) {
      AssertionExtractor assertionExtractor(analyzer);
      assertionExtractor.computeLabelVectorOfEStates();
      assertionExtractor.annotateAst();
      AstAnnotator ara(analyzer->getLabeler());
      ara.annotateAstAttributesAsCommentsBeforeStatements  (sageProject,"ctgen-pre-condition");
      SAWYER_MESG(logger[TRACE]) << "STATUS: Generated assertions."<<endl;
    }

    double arrayUpdateExtractionRunTime=0.0;
    double arrayUpdateSsaNumberingRunTime=0.0;
    double sortingAndIORunTime=0.0;
    double verifyUpdateSequenceRaceConditionRunTime=0.0;

    int verifyUpdateSequenceRaceConditionsResult=-1;
    int verifyUpdateSequenceRaceConditionsTotalLoopNum=-1;
    int verifyUpdateSequenceRaceConditionsParLoopNum=-1;

    if(ctOpt.equiCheck.dumpSortedFileName.size()>0 || ctOpt.equiCheck.dumpNonSortedFileName.size()>0) {
      SAR_MODE sarMode=SAR_SSA;
      if(ctOpt.equiCheck.rewriteSSA) {
	sarMode=SAR_SUBSTITUTE;
      }
      Specialization speci;
      ArrayUpdatesSequence arrayUpdates;
      SAWYER_MESG(logger[TRACE]) <<"STATUS: performing array analysis on STG."<<endl;
      SAWYER_MESG(logger[TRACE]) <<"STATUS: identifying array-update operations in STG and transforming them."<<endl;

      bool useRuleConstSubstitution=ctOpt.equiCheck.ruleConstSubst;
      bool useRuleCommutativeSort=ctOpt.equiCheck.ruleCommutativeSort;
      
      timer.start();
      speci.extractArrayUpdateOperations(analyzer,
          arrayUpdates,
          rewriteSystem,
          useRuleConstSubstitution
          );

      //cout<<"DEBUG: Rewrite1:"<<rewriteSystem.getStatistics().toString()<<endl;
      speci.substituteArrayRefs(arrayUpdates, analyzer->getVariableIdMapping(), sarMode, rewriteSystem);
      //cout<<"DEBUG: Rewrite2:"<<rewriteSystem.getStatistics().toString()<<endl;

      rewriteSystem.setRuleCommutativeSort(useRuleCommutativeSort); // commutative sort only used in substituteArrayRefs
      //cout<<"DEBUG: Rewrite3:"<<rewriteSystem.getStatistics().toString()<<endl;
      speci.substituteArrayRefs(arrayUpdates, analyzer->getVariableIdMapping(), sarMode, rewriteSystem);
      arrayUpdateExtractionRunTime=timer.getTimeDurationAndStop().milliSeconds();

      if(ctOpt.equiCheck.printUpdateInfos) {
        speci.printUpdateInfos(arrayUpdates,analyzer->getVariableIdMapping());
      }
      SAWYER_MESG(logger[TRACE]) <<"STATUS: establishing array-element SSA numbering."<<endl;
      timer.start();
      speci.createSsaNumbering(arrayUpdates, analyzer->getVariableIdMapping());
      arrayUpdateSsaNumberingRunTime=timer.getTimeDurationAndStop().milliSeconds();

      if(ctOpt.equiCheck.dumpNonSortedFileName.size()>0) {
        string filename=ctOpt.equiCheck.dumpNonSortedFileName;
        speci.writeArrayUpdatesToFile(arrayUpdates, filename, sarMode, false);
      }
      if(ctOpt.equiCheck.dumpSortedFileName.size()>0) {
        timer.start();
        string filename=ctOpt.equiCheck.dumpSortedFileName;
        speci.writeArrayUpdatesToFile(arrayUpdates, filename, sarMode, true);
        sortingAndIORunTime=timer.getTimeDurationAndStop().milliSeconds();
      }
      totalRunTime+=arrayUpdateExtractionRunTime+verifyUpdateSequenceRaceConditionRunTime+arrayUpdateSsaNumberingRunTime+sortingAndIORunTime;
    }

    double overallTime =totalRunTime;

    analyzer->printAnalyzerStatistics(totalRunTime, "STG generation and assertion analysis complete");

    if(ctOpt.csvStatsFileName.size()>0) {
      string filename=ctOpt.csvStatsFileName;
      stringstream text;
      text<<"Sizes,"<<pstateSetSize<<", "
          <<eStateSetSize<<", "
          <<transitionGraphSize<<", "
          <<numOfconstraintSets<<", "
          << numOfStdinEStates<<", "
          << numOfStdoutEStates<<", "
          << numOfStderrEStates<<", "
          << numOfFailedAssertEStates<<", "
          << numOfConstEStates<<endl;
      text<<"Memory,"<<pstateSetBytes<<", "
          <<eStateSetBytes<<", "
          <<transitionGraphBytes<<", "
          <<constraintSetsBytes<<", "
          <<totalMemory<<endl;
      text<<"Runtime(readable),"
          <<CodeThorn::readableruntime(frontEndRunTime)<<", "
          <<CodeThorn::readableruntime(initRunTime)<<", "
          <<CodeThorn::readableruntime(analysisRunTime)<<", "
          <<CodeThorn::readableruntime(verifyUpdateSequenceRaceConditionRunTime)<<", "
          <<CodeThorn::readableruntime(arrayUpdateExtractionRunTime)<<", "
          <<CodeThorn::readableruntime(arrayUpdateSsaNumberingRunTime)<<", "
          <<CodeThorn::readableruntime(sortingAndIORunTime)<<", "
          <<CodeThorn::readableruntime(totalRunTime)<<", "
          <<CodeThorn::readableruntime(overallTime)<<endl;
      text<<"Runtime(ms),"
          <<frontEndRunTime<<", "
          <<initRunTime<<", "
          <<normalizationRunTime<<", "
          <<analysisRunTime<<", "
          <<verifyUpdateSequenceRaceConditionRunTime<<", "
          <<arrayUpdateExtractionRunTime<<", "
          <<arrayUpdateSsaNumberingRunTime<<", "
          <<sortingAndIORunTime<<", "
          <<totalRunTime<<", "
          <<overallTime<<endl;
      text<<"hashset-collisions,"
          <<pstateSetMaxCollisions<<", "
          <<eStateSetMaxCollisions<<", "
          <<constraintSetsMaxCollisions<<endl;
      text<<"hashset-loadfactors,"
          <<pstateSetLoadFactor<<", "
          <<eStateSetLoadFactor<<", "
          <<constraintSetsLoadFactor<<endl;
      text<<"threads,"<<analyzer->getNumberOfThreadsToUse()<<endl;
      //    text<<"abstract-and-const-states,"
      //    <<"";

      // iterations (currently only supported for sequential analysis)
      text<<"iterations,";
      if(analyzer->getNumberOfThreadsToUse()==1 && analyzer->getSolver()->getId()==5 && analyzer->getExplorationMode()==EXPL_LOOP_AWARE)
        text<<analyzer->getIterations()<<","<<analyzer->getApproximatedIterations();
      else
        text<<"-1,-1";
      text<<endl;

      // -1: test not performed, 0 (no race conditions), >0: race conditions exist
      text<<"parallelism-stats,";
      if(verifyUpdateSequenceRaceConditionsResult==-1) {
        text<<"sequential";
      } else if(verifyUpdateSequenceRaceConditionsResult==0) {
        text<<"pass";
      } else {
        text<<"fail";
      }
      text<<","<<verifyUpdateSequenceRaceConditionsResult;
      text<<","<<verifyUpdateSequenceRaceConditionsParLoopNum;
      text<<","<<verifyUpdateSequenceRaceConditionsTotalLoopNum;
      text<<endl;

      text<<"rewrite-stats, "<<rewriteSystem.getRewriteStatistics().toCsvString()<<endl;
      text<<"infinite-paths-size,"<<pstateSetSizeInf<<", "
        <<eStateSetSizeInf<<", "
        <<transitionGraphSizeInf<<", "
        <<eStateSetSizeStgInf<<endl;
      //<<numOfconstraintSetsInf<<", "
      //<< numOfStdinEStatesInf<<", "
      //<< numOfStdoutEStatesInf<<", "
      //<< numOfStderrEStatesInf<<", "
      //<< numOfFailedAssertEStatesInf<<", "
      //<< numOfConstEStatesInf<<endl;
      text<<"states & transitions after only-I/O-reduction,"
        <<eStateSetSizeIoOnly<<", "
        <<transitionGraphSizeIoOnly<<endl;

      write_file(filename,text.str());
      cout << "generated "<<filename<<endl;
    }

    if(ltlOpt.ltlStatisticsCSVFileName.size()>0) {
      // content of a line in the .csv file:
      // <#transitions>,<#states>,<#input_states>,<#output_states>,<#error_states>,<#verified_LTL>,<#falsified_LTL>,<#unknown_LTL>
      string filename = ltlOpt.ltlStatisticsCSVFileName;
      write_file(filename,statisticsSizeAndLtl.str());
      cout << "generated "<<filename<<endl;
    }

    if (ltlOpt.cegpra.csvStatsFileName.size()>0) {
      // content of a line in the .csv file:
      // <analyzed_property>,<#transitions>,<#states>,<#input_states>,<#output_states>,<#error_states>,
      // <#analyzed_counterexamples>,<analysis_result(y/n/?)>,<#verified_LTL>,<#falsified_LTL>,<#unknown_LTL>
      string filename = ltlOpt.cegpra.csvStatsFileName;
      write_file(filename,statisticsCegpra.str());
      cout << "generated "<<filename<<endl;
    }


    {
      Visualizer visualizer(analyzer->getLabeler(),analyzer->getVariableIdMapping(),analyzer->getFlow(),analyzer->getPStateSet(),analyzer->getEStateSet(),analyzer->getTransitionGraph());
      if (ctOpt.visualization.icfgFileName.size()>0) {
        string cfgFileName=ctOpt.visualization.icfgFileName;
        DataDependenceVisualizer ddvis(analyzer->getLabeler(),analyzer->getVariableIdMapping(),"none");
        ddvis.setDotGraphName("CFG");
        ddvis.generateDotFunctionClusters(root,analyzer->getCFAnalyzer(),cfgFileName,false);
        cout << "generated "<<cfgFileName<<endl;
      }
      if(ctOpt.visualization.viz) {
        cout << "generating graphviz files:"<<endl;
        visualizer.setOptionMemorySubGraphs(ctOpt.visualization.tg1EStateMemorySubgraphs);
        string dotFile="digraph G {\n";
        dotFile+=visualizer.transitionGraphToDot();
        dotFile+="}\n";
        write_file("transitiongraph1.dot", dotFile);
        cout << "generated transitiongraph1.dot."<<endl;
        string dotFile3=visualizer.foldedTransitionGraphToDot();
        write_file("transitiongraph2.dot", dotFile3);
        cout << "generated transitiongraph2.dot."<<endl;

        string datFile1=(analyzer->getTransitionGraph())->toString(analyzer->getVariableIdMapping());
        write_file("transitiongraph1.dat", datFile1);
        cout << "generated transitiongraph1.dat."<<endl;

        //assert(analyzer->_startFunRoot);
        //analyzer->generateAstNodeInfo(analyzer->_startFunRoot);
        //dotFile=astTermWithNullValuesToDot(analyzer->_startFunRoot);
        SAWYER_MESG(logger[TRACE]) << "Option VIZ: generate ast node info."<<endl;
        analyzer->generateAstNodeInfo(sageProject);
        cout << "generating AST node info ... "<<endl;
        dotFile=AstTerm::functionAstTermsWithNullValuesToDot(sageProject);
        write_file("ast.dot", dotFile);
        cout << "generated ast.dot."<<endl;

        SAWYER_MESG(logger[TRACE]) << "Option VIZ: generating cfg dot file ..."<<endl;
        write_file("cfg_non_clustered.dot", analyzer->getFlow()->toDot(analyzer->getCFAnalyzer()->getLabeler()));
        DataDependenceVisualizer ddvis(analyzer->getLabeler(),analyzer->getVariableIdMapping(),"none");
        ddvis.generateDotFunctionClusters(root,analyzer->getCFAnalyzer(),"cfg.dot",false);
        cout << "generated cfg.dot, cfg_non_clustered.dot"<<endl;
        cout << "=============================================================="<<endl;
      }
      if(ctOpt.visualization.vizTg2) {
        string dotFile3=visualizer.foldedTransitionGraphToDot();
        write_file("transitiongraph2.dot", dotFile3);
        cout << "generated transitiongraph2.dot."<<endl;
      }

      if (ctOpt.visualization. dotIOStg.size()>0) {
        string filename=ctOpt.visualization. dotIOStg;
        cout << "generating dot IO graph file:"<<filename<<endl;
        string dotFile="digraph G {\n";
        dotFile+=visualizer.transitionGraphWithIOToDot();
        dotFile+="}\n";
        write_file(filename, dotFile);
        cout << "=============================================================="<<endl;
      }

      if (ctOpt.visualization.dotIOStgForcedTop.size()>0) {
        string filename=ctOpt.visualization.dotIOStgForcedTop;
        cout << "generating dot IO graph file for an abstract STG:"<<filename<<endl;
        string dotFile="digraph G {\n";
        dotFile+=visualizer.abstractTransitionGraphToDot();
        dotFile+="}\n";
        write_file(filename, dotFile);
        cout << "=============================================================="<<endl;
      }
    }
    // InputPathGenerator
#if 1
    {
      if(ctOpt.rers.iSeqFile.size()>0) {
        int iseqLen=0;
        if(ctOpt.rers.iSeqLength!=-1) {
          iseqLen=ctOpt.rers.iSeqLength;
        } else {
          logger[ERROR] <<"input-sequence file specified, but no sequence length."<<endl;
          exit(1);
        }
        string fileName=ctOpt.rers.iSeqFile;
        SAWYER_MESG(logger[TRACE]) <<"STATUS: computing input sequences of length "<<iseqLen<<endl;
        IOSequenceGenerator iosgen;
        if(ctOpt.rers.iSeqRandomNum!=-1) {
          int randomNum=ctOpt.rers.iSeqRandomNum;
          SAWYER_MESG(logger[TRACE]) <<"STATUS: reducing input sequence set to "<<randomNum<<" random elements."<<endl;
          iosgen.computeRandomInputPathSet(iseqLen,*analyzer->getTransitionGraph(),randomNum);
        } else {
          iosgen.computeInputPathSet(iseqLen,*analyzer->getTransitionGraph());
        }
        SAWYER_MESG(logger[TRACE]) <<"STATUS: generating input sequence file "<<fileName<<endl;
        iosgen.generateFile(fileName);
      } else {
        if(ctOpt.rers.iSeqLength!=-1) {
          logger[ERROR] <<"input sequence length specified without also providing a file name (use option --iseq-file)."<<endl;
          exit(1);
        }
      }
    }
#endif

#if 0
    {
      cout << "EStateSet:\n"<<analyzer->getEStateSet()->toString(analyzer->getVariableIdMapping())<<endl;
      cout << "ConstraintSet:\n"<<analyzer->getConstraintSetMaintainer()->toString()<<endl;
      if(analyzer->variableValueMonitor.isActive())
        cout << "VariableValueMonitor:\n"<<analyzer->variableValueMonitor.toString(analyzer->getVariableIdMapping())<<endl;
      cout << "MAP:"<<endl;
      cout << analyzer->getLabeler()->toString();
    }
#endif

    if (ctOpt.annotateTerms) {
      // TODO: it might be useful to be able to select certain analysis results to be annotated only
      logger[INFO] << "Annotating term representations."<<endl;
      AstTermRepresentationAttribute::attachAstTermRepresentationAttributes(sageProject);
      AstAnnotator ara(analyzer->getLabeler());
      ara.annotateAstAttributesAsCommentsBeforeStatements(sageProject,"codethorn-term-representation");
    }

    if (ctOpt.annotateTerms||ctOpt.generateAssertions) {
      logger[INFO] << "Generating annotated program."<<endl;
      //backend(sageProject);
      sageProject->unparse(0,0);
    }

    // reset terminal
    if(ctOpt.status)
      cout<<color("normal")<<"done."<<endl;

    // main function try-catch
  } catch(const CodeThorn::Exception& e) {
    cerr << "CodeThorn::Exception raised: " << e.what() << endl;
    mfacilities.shutdown();
    return 1;
  } catch(const std::exception& e) {
    cerr<< "std::exception raised: " << e.what() << endl;
    mfacilities.shutdown();
    return 1;
  } catch(char const* str) {
    cerr<< "*Exception raised: " << str << endl;
    mfacilities.shutdown();
    return 1;
  } catch(string str) {
    cerr<< "Exception raised: " << str << endl;
    mfacilities.shutdown();
    return 1;
  } catch(...) {
    cerr<< "Unknown exception raised." << endl;
    mfacilities.shutdown();
    return 1;
  }
  mfacilities.shutdown();
  return 0;
}

