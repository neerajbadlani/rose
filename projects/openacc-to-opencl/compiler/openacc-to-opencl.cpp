
#include "KLT/loop-trees.hpp"
#include "KLT/data.hpp"

#include "KLT/iteration-mapper.hpp"
#include "KLT/loop-mapper.hpp"
#include "KLT/data-flow.hpp"
#include "KLT/cg-config.hpp"
#include "KLT/generator.hpp"
#include "KLT/kernel.hpp"
#include "KLT/mfb-klt.hpp"

#include "KLT/mfb-acc-ocl.hpp"

#include "KLT/dlx-openacc.hpp"

#include "MFB/Sage/driver.hpp"

#include "MDCG/model-builder.hpp"
#include "MDCG/code-generator.hpp"

#include "MFB/Sage/variable-declaration.hpp"

#include <cassert>

#include "sage3basic.h"

typedef ::KLT::Language::OpenCL Language;
typedef ::KLT::Runtime::OpenACC Runtime;
typedef ::DLX::KLT_Annotation< ::DLX::OpenACC::language_t> Annotation;
typedef ::KLT::LoopTrees<Annotation> LoopTrees;
typedef ::KLT::Kernel<Annotation, Language, Runtime> Kernel;

namespace KLT {

class SingleVersionItMapper : public IterationMapper<Annotation, Language::OpenCL, Runtime::OpenACC> {
  private:
    long tile_0;
    long gang;
    long tile_1;
    long worker;
    long tile_2;
    long vector;
    long tile_3;

  public:
    SingleVersionItMapper(long tile_0_, long gang_, long tile_1_, long worker_, long tile_2_, long vector_, long tile_3_) :
      tile_0(tile_0_),
      gang(gang_),
      tile_1(tile_1_),
      worker(worker_),
      tile_2(tile_2_),
      vector(vector_),
      tile_3(tile_3_)
    {}

  private:
    virtual void computeValidShapes(
      LoopTrees<Annotation>::loop_t * loop,
      std::vector<Runtime::OpenACC::loop_shape_t *> & shapes
    ) const {
      shapes.push_back(new Runtime::OpenACC::loop_shape_t(tile_0, gang, tile_1, worker, tile_2, vector, tile_3));
    }
};

}

namespace MDCG {

namespace OpenACC {

struct LoopDesc {
  typedef Runtime::a_loop input_t;

  static SgExpression * createFieldInitializer(
    const MDCG::CodeGenerator & codegen,
    MDCG::Model::field_t element,
    unsigned field_id,
    const input_t & input,
    unsigned file_id
  ) {

    switch (field_id) {
      case 0:
        /// /todo unsigned long tiles[7];
        return SageBuilder::buildAggregateInitializer(
                 SageBuilder::buildExprListExp(
                   SageBuilder::buildLongIntVal(input.tile_0),
                   SageBuilder::buildLongIntVal(input.gang),
                   SageBuilder::buildLongIntVal(input.tile_1),
                   SageBuilder::buildLongIntVal(input.worker),
                   SageBuilder::buildLongIntVal(input.tile_2),
                   SageBuilder::buildLongIntVal(input.vector),
                   SageBuilder::buildLongIntVal(input.tile_3)
                 )
               );
      default:
        assert(false);
    }
  }
};

struct KernelVersion {
  typedef Kernel::a_kernel * input_t;

  static SgExpression * createFieldInitializer(
    const MDCG::CodeGenerator & codegen,
    MDCG::Model::field_t element,
    unsigned field_id,
    const input_t & input,
    unsigned file_id
  ) {
    switch (field_id) {
      case 0:
        /// /todo unsigned long num_gang;
        return SageBuilder::buildIntVal(0);
      case 1:
        /// /todo unsigned long num_worker;
        return SageBuilder::buildIntVal(0);
      case 2:
        /// /todo unsigned long vector_length;
        return SageBuilder::buildIntVal(1);
      case 3:
      {
        /// struct acc_loop_t_ * loops;
        MDCG::Model::type_t type = element->node->type;
        assert(type != NULL && type->node->kind == MDCG::Model::node_t<MDCG::Model::e_model_type>::e_pointer_type);
        type = type->node->base_type;
        assert(type != NULL && type->node->kind == MDCG::Model::node_t<MDCG::Model::e_model_type>::e_class_type);
        return codegen.createArrayPointer<LoopDesc>(type->node->base_class, input->loops.size(), input->loops.begin(), input->loops.end(), file_id);
      }
      case 4:
        /// char * suffix;
        return SageBuilder::buildStringVal(input->kernel_name);
      default:
        assert(false);
    }
  }
};

SgExpression * createArrayOfTypeSize(
  const MDCG::CodeGenerator & codegen,
  const std::list<SgVariableSymbol *> & input,
  std::string array_name,
  unsigned file_id
) {
  SgExprListExp * expr_list = SageBuilder::buildExprListExp();
  SgInitializer * init = SageBuilder::buildAggregateInitializer(expr_list);

  std::list<SgVariableSymbol *>::const_iterator it;
  for (it = input.begin(); it != input.end(); it++)
    expr_list->append_expression(SageBuilder::buildSizeOfOp((*it)->get_type()));

  SgGlobal * global_scope_across_files = codegen.getDriver().project->get_globalScopeAcrossFiles();
  assert(global_scope_across_files != NULL);
  SgTypedefSymbol * size_t_symbol = SageInterface::lookupTypedefSymbolInParentScopes("size_t", global_scope_across_files);
  assert(size_t_symbol != NULL);
  SgType * size_t_type = isSgType(size_t_symbol->get_type());
  assert(size_t_type != NULL);
  size_t_type = SageBuilder::buildArrayType(size_t_type, SageBuilder::buildIntVal(input.size()));

  MFB::Sage<SgVariableDeclaration>::object_desc_t var_decl_desc(array_name, size_t_type, init, NULL, file_id, false, true);
  MFB::Sage<SgVariableDeclaration>::build_result_t var_decl_res = codegen.getDriver().build<SgVariableDeclaration>(var_decl_desc);

  return SageBuilder::buildVarRefExp(var_decl_res.symbol);
}

struct KernelDesc {
  typedef Kernel * input_t;

  static SgExpression * createFieldInitializer(
    const MDCG::CodeGenerator & codegen,
    MDCG::Model::field_t element,
    unsigned field_id,
    const input_t & input,
    unsigned file_id
  ) {
    const Kernel::arguments_t & args = input->getArguments();
    const std::vector<Kernel::a_kernel *> & versions = input->getKernels();

    std::ostringstream names_suffix;
    names_suffix << "_" << input;

    switch (field_id) {
      case 0:
        /// unsigned id;
        return SageBuilder::buildIntVal(input->id);
      case 1:
        /// size_t num_params;
        return SageBuilder::buildIntVal(args.parameters.size());
      case 2:
        /// size_t * size_params;
        return createArrayOfTypeSize(codegen, args.parameters, std::string("param_sizes") + names_suffix.str(), file_id);
      case 3:
        /// size_t num_scalars;
        return SageBuilder::buildIntVal(args.scalars.size());
      case 4:
        /// size_t * size_scalars;
        return createArrayOfTypeSize(codegen, args.scalars, std::string("scalar_sizes") + names_suffix.str(), file_id);
      case 5:
        /// size_t num_datas;
        return SageBuilder::buildIntVal(args.datas.size());
      case 6:
        /// size_t num_loops;
        return SageBuilder::buildIntVal(input->num_loops);
      case 7:
        /// unsigned num_versions;
        return SageBuilder::buildIntVal(versions.size());
      case 8:
      {
        /// acc_kernel_version_t * versions;
        MDCG::Model::type_t type = element->node->type;
        assert(type != NULL && type->node->kind == MDCG::Model::node_t<MDCG::Model::e_model_type>::e_pointer_type);
        type = type->node->base_type;
        assert(type != NULL && type->node->kind == MDCG::Model::node_t<MDCG::Model::e_model_type>::e_typedef_type);
        type = type->node->base_type;
        assert(type != NULL && type->node->kind == MDCG::Model::node_t<MDCG::Model::e_model_type>::e_pointer_type);
        type = type->node->base_type;
        assert(type != NULL && type->node->kind == MDCG::Model::node_t<MDCG::Model::e_model_type>::e_class_type);
        return codegen.createPointerArrayPointer<KernelVersion>(type->node->base_class, versions.size(), versions.begin(), versions.end(), file_id);
      }
      case 9:
      {
        /// \todo acc_loop_splitter_t * splitted_loop;
        return SageBuilder::buildIntVal(0);
      }
      default:
        assert(false);
    }
  }
};

struct RegionDesc {
  struct input_t {
    unsigned id;
    std::string file;
    std::set<std::list<Kernel *> > kernel_lists;
  };

  static SgExpression * createFieldInitializer(
    const MDCG::CodeGenerator & codegen,
    MDCG::Model::field_t element,
    unsigned field_id,
    const input_t & input,
    unsigned file_id
  ) {
    assert(input.kernel_lists.size() == 1);
    const std::list<Kernel *> & kernels = *(input.kernel_lists.begin());

    switch (field_id) {
      case 0:
        /// unsigned id;
        return SageBuilder::buildIntVal(input.id);
      case 1:
        /// char * file;
        return SageBuilder::buildStringVal(input.file.c_str());
      case 2:
        /// \todo size_t num_options;
        return SageBuilder::buildIntVal(0);
      case 3:
        /// \todo char ** options;
        return SageBuilder::buildIntVal(0);
      case 4:
        /// size_t num_kernels;
        return SageBuilder::buildIntVal(kernels.size());
      case 5:
      {
        /// acc_kernel_desc_t * kernels;
        MDCG::Model::type_t type = element->node->type;
        assert(type != NULL && type->node->kind == MDCG::Model::node_t<MDCG::Model::e_model_type>::e_pointer_type);
        type = type->node->base_type;
        assert(type != NULL && type->node->kind == MDCG::Model::node_t<MDCG::Model::e_model_type>::e_typedef_type);
        type = type->node->base_type;
        assert(type != NULL && type->node->kind == MDCG::Model::node_t<MDCG::Model::e_model_type>::e_pointer_type);
        type = type->node->base_type;
        assert(type != NULL && type->node->kind == MDCG::Model::node_t<MDCG::Model::e_model_type>::e_class_type);
        return codegen.createPointerArrayPointer<KernelDesc>(type->node->base_class, kernels.size(), kernels.begin(), kernels.end(), file_id);
      }
      case 6:
        /// \todo size_t num_devices;
        return SageBuilder::buildIntVal(1);
      case 7:
        /// \todo struct { acc_device_t kind; size_t num; } * devices;
        return SageBuilder::buildIntVal(0); // NULL
      case 8:
        /// \todo size_t num_distributed_datas;
        return SageBuilder::buildIntVal(0);
      case 9:
        /// \todo struct acc_data_distribution_t_ * data_distributions;
        return SageBuilder::buildIntVal(0); // NULL
      default:
        assert(false);
    }
  }
};

}

}

unsigned readOpenaccModel(MDCG::ModelBuilder & model_builder, const std::string & libopenacc_dir) {
  unsigned openacc_model = model_builder.create();

  model_builder.add(openacc_model, "region",       libopenacc_dir + "/OpenACC/internal", "h");
  model_builder.add(openacc_model, "kernel",       libopenacc_dir + "/OpenACC/internal", "h");
  model_builder.add(openacc_model, "loop",         libopenacc_dir + "/OpenACC/internal", "h");
  model_builder.add(openacc_model, "api",          libopenacc_dir + "/OpenACC/device",   "cl");
//model_builder.add(openacc_model, "compiler",     libopenacc_dir + "include/OpenACC/internal", "h");
//model_builder.add(openacc_model, "data-env",     libopenacc_dir + "include/OpenACC/internal", "h");
//model_builder.add(openacc_model, "init",         libopenacc_dir + "include/OpenACC/internal", "h");
//model_builder.add(openacc_model, "mem-manager",  libopenacc_dir + "include/OpenACC/internal", "h");
//model_builder.add(openacc_model, "opencl-debug", libopenacc_dir + "include/OpenACC/internal", "h");
//model_builder.add(openacc_model, "opencl-init",  libopenacc_dir + "include/OpenACC/internal", "h");
//model_builder.add(openacc_model, "runtime",      libopenacc_dir + "include/OpenACC/internal", "h");
//model_builder.add(openacc_model, "data-env",     libopenacc_dir + "include/OpenACC/private",  "h");
//model_builder.add(openacc_model, "debug",        libopenacc_dir + "include/OpenACC/private",  "h");
//model_builder.add(openacc_model, "init",         libopenacc_dir + "include/OpenACC/private",  "h");
//model_builder.add(openacc_model, "kernel",       libopenacc_dir + "include/OpenACC/private",  "h");
//model_builder.add(openacc_model, "loop",         libopenacc_dir + "include/OpenACC/private",  "h");
//model_builder.add(openacc_model, "region",       libopenacc_dir + "include/OpenACC/private",  "h");
//model_builder.add(openacc_model, "runtime",      libopenacc_dir + "include/OpenACC/private",  "h");
//model_builder.add(openacc_model, "openacc",      libopenacc_dir + "include/OpenACC/",         "h");

  return openacc_model;
}

int main(int argc, char ** argv) {

  // Arguments
  assert(argc == 7);
  std::string libopenacc_dir(argv[2]);
  std::string opencl_dir(argv[3]);
  long t0 = atol(argv[4]);
  long t1 = atol(argv[5]);
  long t2 = atol(argv[6]);

  // Build a default ROSE project
  SgProject * project = new SgProject::SgProject();
  { // Add default command line to an empty project
    std::vector<std::string> arglist;
      arglist.push_back("c++");
      arglist.push_back("-DSKIP_ROSE_BUILTIN_DECLARATIONS");
      arglist.push_back(std::string("-I") + libopenacc_dir);
      arglist.push_back(std::string("-I") + opencl_dir + "/include/");
      arglist.push_back("-c");
    project->set_originalCommandLineArgumentList (arglist);
  }

  // Initialize DLX for OpenACC
  DLX::OpenACC::language_t::init();

  // Initialize MFB to use with KLT
  MFB::KLT_Driver driver(project);

  // Initialize MDCG's ModelBuilder and Code Generator
  MDCG::ModelBuilder model_builder(driver);
  MDCG::CodeGenerator codegen(driver);

  // Initialize KLT's Generator
  KLT::Generator<Annotation, Language, Runtime, MFB::KLT_Driver> generator(driver, "kernels.cl");

  // Read input LoopTrees
  LoopTrees loop_trees;
  loop_trees.read(argv[1]);

  // Read OpenACC Model
  unsigned model = readOpenaccModel(model_builder, libopenacc_dir);

  unsigned host_data_file_id = driver.add(boost::filesystem::path(std::string("host-data.c")));
  driver.setUnparsedFile(host_data_file_id);

  // Load OpenACC API for KLT
  KLT::Runtime::OpenACC::loadAPI(driver, libopenacc_dir);

  // Create a Code Generation Configuration
  KLT::CG_Config<Annotation, Language, Runtime> cg_config(
      new KLT::LoopMapper<Annotation, Language, Runtime>(),
      new KLT::SingleVersionItMapper(t0, 0, t1, 0, t2, 1, 1),
      new KLT::DataFlow<Annotation, Language, Runtime>()
  );

  // Call the generator
  MDCG::OpenACC::RegionDesc::input_t input;
  input.id = 0;
  input.file = std::string("kernels.cl");
  generator.generate(loop_trees, input.kernel_lists, cg_config);

  // Get model element for Region Descriptor
  std::set<MDCG::Model::class_t> classes;
  model_builder.get(model).lookup<MDCG::Model::class_t>("acc_region_desc_t_", classes);
  assert(classes.size() == 1);
  MDCG::Model::class_t region_desc_class = *(classes.begin());

  codegen.addDeclaration<MDCG::OpenACC::RegionDesc>(region_desc_class, input, host_data_file_id, "regions");

  project->unparse(); // Cannot call the backend directly because of OpenCL files. There is a warning when trying, just have to trace it.

  return 0;
}

