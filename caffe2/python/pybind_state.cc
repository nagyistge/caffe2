#include "pybind_state.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "caffe2/core/db.h"
#include "caffe2/core/predictor.h"

namespace caffe2 {

namespace py = pybind11;

// gWorkspaces allows us to define and switch between multiple workspaces in
// Python.
static std::map<std::string, std::unique_ptr<Workspace>> gWorkspaces;
// gWorkspace is the pointer to the current workspace. The ownership is kept
// by the gWorkspaces map.
static Workspace* gWorkspace = nullptr;
static std::string gCurrentWorkspaceName;

BlobFetcherBase::~BlobFetcherBase() {}
BlobFeederBase::~BlobFeederBase() {}

CAFFE_DEFINE_TYPED_REGISTRY(BlobFetcherRegistry, CaffeTypeId, BlobFetcherBase);
CAFFE_DEFINE_TYPED_REGISTRY(BlobFeederRegistry, int, BlobFeederBase);

REGISTER_BLOB_FETCHER((TypeMeta::Id<TensorCPU>()), TensorFetcher<CPUContext>);
REGISTER_BLOB_FEEDER(CPU, TensorFeeder<CPUContext>);

class StringFetcher : public BlobFetcherBase {
 public:
  py::object Fetch(const Blob& blob) override {
    return py::str(blob.Get<string>());
  }
};
REGISTER_BLOB_FETCHER((TypeMeta::Id<string>()), StringFetcher);

static_assert(
    sizeof(int) == sizeof(int32_t),
    "We make an assumption that int is always int32 for numpy "
    "type mapping.");
int CaffeToNumpyType(const TypeMeta& meta) {
  static std::map<CaffeTypeId, int> numpy_type_map{
      {TypeMeta::Id<bool>(), NPY_BOOL},
      {TypeMeta::Id<double>(), NPY_DOUBLE},
      {TypeMeta::Id<float>(), NPY_FLOAT},
      {TypeMeta::Id<float16>(), NPY_FLOAT16},
      {TypeMeta::Id<int>(), NPY_INT},
      {TypeMeta::Id<int8_t>(), NPY_INT8},
      {TypeMeta::Id<int16_t>(), NPY_INT16},
      {TypeMeta::Id<int64_t>(), NPY_LONGLONG},
      {TypeMeta::Id<uint8_t>(), NPY_UINT8},
      {TypeMeta::Id<uint16_t>(), NPY_UINT16},
      {TypeMeta::Id<std::string>(), NPY_OBJECT},
      // Note: Add more types here.
  };
  const auto it = numpy_type_map.find(meta.id());
  return it == numpy_type_map.end() ? -1 : it->second;
}

const TypeMeta& NumpyTypeToCaffe(int numpy_type) {
  static std::map<int, TypeMeta> caffe_type_map{
      {NPY_BOOL, TypeMeta::Make<bool>()},
      {NPY_DOUBLE, TypeMeta::Make<double>()},
      {NPY_FLOAT, TypeMeta::Make<float>()},
      {NPY_FLOAT16, TypeMeta::Make<float16>()},
      {NPY_INT, TypeMeta::Make<int>()},
      {NPY_INT8, TypeMeta::Make<int8_t>()},
      {NPY_INT16, TypeMeta::Make<int16_t>()},
      {NPY_INT64, TypeMeta::Make<int64_t>()},
      {NPY_LONG,
       sizeof(long) == sizeof(int) ? TypeMeta::Make<int>()
                                   : TypeMeta::Make<int64_t>()},
      {NPY_LONGLONG, TypeMeta::Make<int64_t>()},
      {NPY_UINT8, TypeMeta::Make<uint8_t>()},
      {NPY_UINT16, TypeMeta::Make<uint16_t>()},
      {NPY_OBJECT, TypeMeta::Make<std::string>()},
      // Note: Add more types here.
  };
  static TypeMeta unknown_type;
  const auto it = caffe_type_map.find(numpy_type);
  return it == caffe_type_map.end() ? unknown_type : it->second;
}

template <typename Registry>
std::function<const char*(const string&)> DefinitionGetter(
    const Registry* registry) {
  return [registry](const string& name) { return registry->HelpMessage(name); };
}

void switchWorkspaceInternal(const std::string& name, bool create_if_missing) {
  if (gWorkspaces.count(name)) {
    gCurrentWorkspaceName = name;
    gWorkspace = gWorkspaces[name].get();
    return;
  }

  CAFFE_ENFORCE(create_if_missing);
  std::unique_ptr<Workspace> new_workspace(new Workspace());
  gWorkspace = new_workspace.get();
  gWorkspaces.insert(std::make_pair(name, std::move(new_workspace)));
  gCurrentWorkspaceName = name;
}

void addObjectMethods(py::module& m) {
  py::class_<NetBase>(m, "Net").def("run", [](NetBase* net) {
    py::gil_scoped_release g;
    CAFFE_ENFORCE(net->Run());
  });

  py::class_<Blob>(m, "Blob")
      .def(
          "serialize",
          [](const Blob& blob, const std::string& name) -> py::bytes {
            return blob.Serialize(name);
          })
      .def(
          "deserialize",
          [](Blob* blob, py::bytes serialized) {
            CAFFE_ENFORCE(blob->Deserialize(serialized));
          })
      .def(
          "fetch",
          [](const Blob& blob) {
            auto fetcher = CreateFetcher(blob.meta().id());
            CAFFE_ENFORCE(
                fetcher,
                "Could not fetch for blob of type: ",
                blob.meta().name());
            return fetcher->Fetch(blob);
          })
      .def(
          "_feed",
          [](Blob* blob,
             const py::object& arg,
             const py::object device_option) {
            DeviceOption option;
            if (device_option != py::none()) {
              // If we have a device option passed in, read it.
              CAFFE_ENFORCE(option.ParseFromString(
                  py::bytes(device_option).cast<std::string>()));
            }
            if (PyArray_Check(arg.ptr())) { // numpy array
              PyArrayObject* array =
                  reinterpret_cast<PyArrayObject*>(arg.ptr());
              auto feeder = CreateFeeder(option.device_type());
              CAFFE_ENFORCE(
                  feeder, "Unknown device type encountered in FeedBlob.");
              feeder->Feed(option, array, blob);
              return true;
            }

            if (PyString_Check(arg.ptr())) { // string
              *blob->GetMutable<std::string>() = arg.cast<std::string>();
              return true;
            }
            CAFFE_THROW(
                "Unexpected type of argument - only numpy array or string are "
                "supported for feeding");
          },
          "Feed an input array or string, with the (optional) DeviceOption",
          py::arg("arg"),
          py::arg("device_option") = py::none());

  py::class_<Workspace>(m, "Workspace")
      .def(py::init<>())
      .def_property_readonly(
          "nets",
          [](Workspace* self) {
            CHECK_NOTNULL(self);
            std::map<std::string, py::object> nets;
            for (const auto& name : self->Nets()) {
              LOG(INFO) << "name: " << name;
              nets[name] = py::cast(
                  self->GetNet(name),
                  py::return_value_policy::reference_internal);
            }
            return nets;
          })
      .def_property_readonly(
          "blobs",
          [](Workspace* self) {
            CHECK_NOTNULL(self);
            std::map<std::string, py::object> blobs;
            for (const auto& name : self->Blobs()) {
              blobs[name] = py::cast(
                  self->GetBlob(name),
                  py::return_value_policy::reference_internal);
            }
            return blobs;
          })
      .def(
          "_create_net",
          [](Workspace* self, py::bytes def) -> py::object {
            caffe2::NetDef proto;
            CAFFE_ENFORCE(proto.ParseFromString(def));
            auto* net = self->CreateNet(proto);
            CAFFE_ENFORCE(net);
            return py::cast(net, py::return_value_policy::reference_internal);
          })
      .def(
          "create_blob",
          [](Workspace* self, const std::string& name) -> py::object {
            auto* blob = self->CreateBlob(name);
            return py::cast(blob, py::return_value_policy::reference_internal);
          })
      .def(
          "_run_net",
          [](Workspace* self, py::bytes def) {
            caffe2::NetDef proto;
            CAFFE_ENFORCE(proto.ParseFromString(def));
            py::gil_scoped_release g;
            CAFFE_ENFORCE(self->RunNetOnce(proto));
          })
      .def(
          "_run_operator",
          [](Workspace* self, py::bytes def) {
            caffe2::OperatorDef proto;
            CAFFE_ENFORCE(proto.ParseFromString(def));
            py::gil_scoped_release g;
            CAFFE_ENFORCE(self->RunOperatorOnce(proto));
          })
      .def("_run_plan", [](Workspace* self, py::bytes def) {
        caffe2::PlanDef proto;
        CAFFE_ENFORCE(proto.ParseFromString(def));
        py::gil_scoped_release g;
        CAFFE_ENFORCE(self->RunPlan(proto));
      });

  // Gradients
  py::class_<GradientWrapper>(m, "GradientWrapper")
      .def(py::init<>())
      .def_readwrite("dense", &GradientWrapper::dense_)
      .def_readwrite("indices", &GradientWrapper::indices_)
      .def_readwrite("values", &GradientWrapper::values_)
      .def("is_sparse", &GradientWrapper::IsSparse)
      .def("is_dense", &GradientWrapper::IsDense)
      .def("is_empty", &GradientWrapper::IsEmpty);

  m.def(
      "get_gradient_defs",
      [](const py::bytes& op_def,
         std::vector<GradientWrapper> output_gradients) {
        OperatorDef def;
        CAFFE_ENFORCE(def.ParseFromString(op_def));
        CAFFE_ENFORCE(caffe2::GradientRegistry()->Has(def.type()));
        const auto& meta = GetGradientForOp(def, output_gradients);
        std::vector<py::bytes> grad_ops;
        for (const auto& op : meta.ops_) {
          grad_ops.push_back(op.SerializeAsString());
        }
        return std::pair<std::vector<py::bytes>, std::vector<GradientWrapper>>{
            grad_ops, meta.g_input_};
      });

  // DB
  py::class_<db::Transaction>(m, "Transaction")
      .def("put", &db::Transaction::Put)
      .def("commit", &db::Transaction::Commit);
  py::class_<db::Cursor>(m, "Cursor")
      .def("supports_seak", &db::Cursor::SupportsSeek)
      .def("seek_to_first", &db::Cursor::SeekToFirst)
      .def("next", &db::Cursor::Next)
      .def("key", &db::Cursor::key)
      .def("value", &db::Cursor::value)
      .def("valid", &db::Cursor::Valid);
  py::enum_<db::Mode>(m, "Mode")
      .value("read", db::Mode::READ)
      .value("write", db::Mode::WRITE)
      .value("new", db::Mode::NEW)
      .export_values();
  py::class_<db::DB /*, std::unique_ptr<DB>*/>(m, "DB")
      .def("new_transaction", &db::DB::NewTransaction)
      .def("new_cursor", &db::DB::NewCursor)
      .def("close", &db::DB::Close);
  m.def("create_db", &db::CreateDB);

  // OpSchema
  py::class_<OpSchema>(m, "OpSchema")
      .def_property_readonly("file", &OpSchema::file)
      .def_property_readonly("line", &OpSchema::line)
      .def_property_readonly(
          "doc", &OpSchema::doc, py::return_value_policy::reference)
      .def_property_readonly("arg_desc", &OpSchema::arg_desc)
      .def_property_readonly("input_desc", &OpSchema::input_desc)
      .def_property_readonly("output_desc", &OpSchema::output_desc)
      // Note: this does not work yet, we will need to figure out how to pass
      // protobuf objects.
      .def("infer_tensor", &OpSchema::InferTensor)
      .def_static(
          "get", &OpSchemaRegistry::Schema, py::return_value_policy::reference)
      .def_static(
          "get_cpu_impl",
          DefinitionGetter(CPUOperatorRegistry()),
          py::return_value_policy::reference)
      .def_static(
          "get_cuda_impl",
          DefinitionGetter(CUDAOperatorRegistry()),
          py::return_value_policy::reference)
      .def_static(
          "get_gradient_impl",
          DefinitionGetter(GradientRegistry()),
          py::return_value_policy::reference);

  py::class_<Predictor>(m, "Predictor")
      .def(
          "__init__",
          [](Predictor& instance, py::bytes init_net, py::bytes predict_net) {
            NetDef init_net_, predict_net_;
            CAFFE_ENFORCE(init_net_.ParseFromString(init_net));
            CAFFE_ENFORCE(predict_net_.ParseFromString(predict_net));
            new (&instance) Predictor(init_net_, predict_net_);
          });
}

void addGlobalMethods(py::module& m) {
  m.def("global_init", [](std::vector<std::string> args) -> void {
    int argc = args.size();
    std::vector<char*> argv;
    for (auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.data()));
    }
    char** pargv = argv.data();
    CAFFE_ENFORCE(caffe2::GlobalInit(&argc, &pargv));
  });

  m.def("registered_operators", []() {
    std::set<string> all_keys;

    // CPU operators
    for (const auto& name : caffe2::CPUOperatorRegistry()->Keys()) {
      all_keys.insert(name);
    }
    // CUDA operators
    for (const auto& name : caffe2::CUDAOperatorRegistry()->Keys()) {
      all_keys.insert(name);
    }
    // Ensure we are lexicographically ordered.
    std::vector<std::string> keys;
    for (const auto& key : all_keys) {
      keys.push_back(key);
    }
    return keys;
  });
  m.def("on_module_exit", []() { gWorkspaces.clear(); });
  m.def(
      "switch_workspace",
      [](const std::string& name, py::object create_if_missing) {
        if (create_if_missing == py::none()) {
          return switchWorkspaceInternal(name, false);
        }
        return switchWorkspaceInternal(name, create_if_missing.cast<bool>());
      },
      "Switch to the specified workspace, creating if necessary",
      py::arg("name"),
      py::arg("create_if_missing") = py::none());
  m.def(
      "reset_workspace",
      [](const py::object& root_folder) {
        VLOG(1) << "Resetting workspace.";
        if (root_folder == py::none()) {
          gWorkspaces[gCurrentWorkspaceName].reset(new Workspace());
        } else {
          gWorkspaces[gCurrentWorkspaceName].reset(
              new Workspace(root_folder.cast<std::string>()));
        }
        gWorkspace = gWorkspaces[gCurrentWorkspaceName].get();
        return true;
      },
      "Reset the workspace",
      py::arg("root_folder") = py::none());
  m.def("root_folder", []() {
    CAFFE_ENFORCE(gWorkspace);
    return gWorkspace->RootFolder();
  });
  m.def("current_workspace", []() { return gCurrentWorkspaceName; });
  m.def("workspaces", []() {
    std::vector<std::string> names;
    for (const auto& kv : gWorkspaces) {
      names.push_back(kv.first);
    }
    return names;
  });
  m.def("blobs", []() {
    CAFFE_ENFORCE(gWorkspace);
    return gWorkspace->Blobs();
  });
  m.def("has_blob", [](const std::string& name) {
    CAFFE_ENFORCE(gWorkspace);
    return gWorkspace->HasBlob(name);
  });
  m.def("create_net", [](py::bytes net_def) {
    caffe2::NetDef proto;
    CAFFE_ENFORCE(proto.ParseFromString(net_def));
    CAFFE_ENFORCE(gWorkspace->CreateNet(proto));
    return true;
  });
  m.def("run_net", [](const std::string& name) {
    CAFFE_ENFORCE(gWorkspace);
    CAFFE_ENFORCE(gWorkspace->GetNet(name));
    py::gil_scoped_release g;
    CAFFE_ENFORCE(gWorkspace->RunNet(name));
    return true;
  });
  m.def(
      "benchmark_net",
      [](const std::string& name,
         size_t warmup_runs,
         size_t main_runs,
         bool run_individual) {
        CAFFE_ENFORCE(gWorkspace);
        auto* net = gWorkspace->GetNet(name);
        CAFFE_ENFORCE(net);
        py::gil_scoped_release g;
        vector<float> stat =
            net->TEST_Benchmark(warmup_runs, main_runs, run_individual);
        return stat;
      });

  m.def("delete_net", [](const std::string& name) {
    CAFFE_ENFORCE(gWorkspace);
    gWorkspace->DeleteNet(name);
    return true;
  });
  m.def("nets", []() { return gWorkspace->Nets(); });
  m.def("run_operator_once", [](const py::bytes& op_def) {
    CAFFE_ENFORCE(gWorkspace);
    OperatorDef def;
    CAFFE_ENFORCE(def.ParseFromString(op_def));
    py::gil_scoped_release g;
    CAFFE_ENFORCE(gWorkspace->RunOperatorOnce(def));
    return true;
  });
  m.def("run_net_once", [](const py::bytes& net_def) {
    CAFFE_ENFORCE(gWorkspace);
    NetDef def;
    CAFFE_ENFORCE(def.ParseFromString(net_def));
    py::gil_scoped_release g;
    CAFFE_ENFORCE(gWorkspace->RunNetOnce(def));
    return true;
  });
  m.def("run_plan", [](const py::bytes& plan_def) {
    CAFFE_ENFORCE(gWorkspace);
    PlanDef def;
    CAFFE_ENFORCE(def.ParseFromString(plan_def));
    py::gil_scoped_release g;
    CAFFE_ENFORCE(gWorkspace->RunPlan(def));
    return true;
  });
  m.def("create_blob", [](const std::string& name) {
    CAFFE_ENFORCE(gWorkspace);
    CAFFE_ENFORCE(gWorkspace->CreateBlob(name));
    return true;
  });
  m.def("fetch_blob", [](const std::string& name) -> py::object {
    CAFFE_ENFORCE(gWorkspace->HasBlob(name), "Can't find blob: ", name);
    const caffe2::Blob& blob = *(gWorkspace->GetBlob(name));
    auto fetcher = CreateFetcher(blob.meta().id());
    if (fetcher) {
      return fetcher->Fetch(blob);
    } else {
      // If there is no fetcher registered, return a metainfo string.
      // If all branches failed, we will return a metainfo string.
      std::stringstream ss;
      ss << caffe2::string(name) << ", a C++ native class of type "
         << blob.TypeName() << ".";
      return py::str(ss.str());
    }
  });
  m.def(
      "feed_blob",
      [](const std::string& name, py::object arg, py::object device_option) {
        DeviceOption option;
        if (device_option != py::none()) {
          // If we have a device option passed in, read it.
          CAFFE_ENFORCE(option.ParseFromString(
              py::bytes(device_option).cast<std::string>()));
        }
        auto* blob = gWorkspace->CreateBlob(name);
        if (PyArray_Check(arg.ptr())) { // numpy array
          PyArrayObject* array = reinterpret_cast<PyArrayObject*>(arg.ptr());
          auto feeder = CreateFeeder(option.device_type());
          CAFFE_ENFORCE(feeder, "Unknown device type encountered in FeedBlob.");
          feeder->Feed(option, array, blob);
          return true;
        }

        if (PyString_Check(arg.ptr())) { // string
          *blob->GetMutable<std::string>() = arg.cast<std::string>();
          return true;
        }
        CAFFE_THROW(
            "Unexpected type of argument - only numpy array or string are "
            "supported for feeding");
        return false;
      },
      "",
      py::arg("name"),
      py::arg("arg"),
      py::arg("device_option") = py::none());
  m.def("serialize_blob", [](const std::string& name) {
    CAFFE_ENFORCE(gWorkspace);
    auto* blob = gWorkspace->GetBlob(name);
    CAFFE_ENFORCE(blob);
    return py::bytes(blob->Serialize(name));
  });
  m.def(
      "deserialize_blob",
      [](const std::string& name, const py::bytes& serialized) {
        CAFFE_ENFORCE(gWorkspace);
        auto* blob = gWorkspace->CreateBlob(name);
        CAFFE_ENFORCE(blob->Deserialize(serialized.cast<std::string>()));
      });

  auto initialize = [&]() {
    // Initialization of the module
    ([]() {
      // This is a workaround so we can deal with numpy's import_array behavior.
      // Despite the fact that you may think import_array() is a function call,
      // it is defined as a macro (as of 1.10).
      import_array();
    })();
    // Single threaded, so safe
    static bool initialized = false;
    if (initialized) {
      return;
    }
    // We will create a default workspace for us to run stuff.
    switchWorkspaceInternal("default", true);
    gCurrentWorkspaceName = "default";
    initialized = true;
  };

  initialize();
};

PYBIND11_PLUGIN(caffe2_pybind11_state) {
  py::module m(
      "caffe2_pybind11_state",
      "pybind11 stateful interface to Caffe2 workspaces");

  addGlobalMethods(m);
  addObjectMethods(m);
  return m.ptr();
}
}
