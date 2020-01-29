#include <torch/csrc/distributed/rpc/py_rref.h>

#include <torch/csrc/distributed/rpc/python_functions.h>
#include <torch/csrc/distributed/rpc/python_rpc_handler.h>
#include <torch/csrc/distributed/rpc/rref_context.h>
#include <torch/csrc/jit/pybind_utils.h>

namespace torch {
namespace distributed {
namespace rpc {
///////////////////////////  PyRRef  //////////////////////////////////

PyRRef::PyRRef(std::shared_ptr<RRef> rref) : rref_(std::move(rref)) {
  TORCH_CHECK(rref_, "PyRRef must not wrap nullptr");
}

PyRRef::PyRRef(const py::object& value)
    : PyRRef([&value]() {
        auto rref =
            RRefContext::getInstance().createOwnerRRef(PyObjectType::get());
        py::object copy(value); // increases refcount
        IValue py_ivalue = jit::toIValue(std::move(copy), PyObjectType::get());
        rref->setValue(std::move(py_ivalue));
        return rref;
      }()) {}

bool PyRRef::isOwner() const {
  return rref_->isOwner();
}

WorkerInfo PyRRef::owner() const {
  return RRefContext::getInstance().agent()->getWorkerInfo(rref_->owner());
}

py::object PyRRef::toHere() {
  if (rref_->isOwner()) {
    return localValue();
  } else {
    if (rref_->isPyObj()) {
      // UserRRef<py::object>::toHere() calls python_rpc_handler which acquires
      // GIL.
      return jit::toPyObject(
          std::static_pointer_cast<UserRRef>(rref_)->toHere());
    } else {
      IValue value = std::static_pointer_cast<UserRRef>(rref_)->toHere();

      {
        // acquiring GIL as torch::jit::toPyObject creates new py::object
        // without grabbing the GIL.
        pybind11::gil_scoped_acquire ag;
        return torch::jit::toPyObject(std::move(value));
      }
    }
  }
}

py::object PyRRef::localValue() {
  TORCH_CHECK(
      rref_->isOwner(),
      "Cannot call localValue() on a non-local reference. Call it on ",
      owner().name_);

  if (rref_->isPyObj()) {
    const py::object& value = jit::toPyObject(
        std::dynamic_pointer_cast<OwnerRRef>(rref_)->getValue());
    PythonRpcHandler::getInstance().handleException(value);
    {
      // acquiring GIL as the return statement construct a new py::object from
      // a const reference.
      pybind11::gil_scoped_acquire ag;
      return value;
    }
  } else {
    auto value = std::dynamic_pointer_cast<OwnerRRef>(rref_)->getValue();
    {
      // acquiring GIL as torch::jit::toPyObject creates new py::object without
      // grabbing the GIL.
      pybind11::gil_scoped_acquire ag;
      return torch::jit::toPyObject(std::move(value));
    }
  }
}

std::string PyRRef::str() const {
  std::stringstream ss;
  if (rref_->isOwner()) {
    ss << "OwnerRRef(" << rref_->rrefId() << ")";
  } else {
    ss << "UserRRef(RRefId = " << rref_->rrefId()
       << ", ForkId = " << std::static_pointer_cast<UserRRef>(rref_)->forkId()
       << ")";
  }
  return ss.str();
}

py::tuple PyRRef::pickle() const {
  auto& ctx = RRefContext::getInstance();
  // TODO: use a dispatch table to pickle/unpickle an RRef, and only only
  // install the dispatch table only when there are indeed RPC activities. As
  // a counter example, checkpointing a model with RRefs should not trigger
  // forks to be added as a fork or a child.
  auto rrefForkData = ctx.prepareChildFork(rref_);
  return rrefforkdata::toPyTuple(rrefForkData);
}

PyRRef PyRRef::unpickle(const py::tuple& pyTuple) {
  auto& ctx = RRefContext::getInstance();
  auto rrefForkData = rrefforkdata::fromPyTuple(pyTuple);
  std::shared_ptr<RRef> rref = nullptr;
  TypePtr rrefType =
      PythonRpcHandler::getInstance().parseTypeFromStr(rrefForkData.typeStr_);
  rref = ctx.getOrCreateRRef(rrefForkData, rrefType);

  ctx.notifyOwnerAndParentOfFork(
      rrefForkData.forkId_, rrefForkData.parent_, rref);
  return PyRRef(std::move(rref));
}

namespace rrefforkdata {

namespace {
constexpr int OWNER_IDX = 0; // index of ownerId in the tuple
constexpr int RREFID_ON_IDX = 1; // index of RRefId.createdOn_ in the tuple
constexpr int RREFID_ID_IDX = 2; // index of RRefId.localId_ in the tuple
constexpr int FORKID_ON_IDX = 3; // index of ForkId.createdOn_ in the tuple
constexpr int FORKID_ID_IDX = 4; // index of ForkId.localId_ in the tuple
constexpr int PARENT_IDX = 5; // index of parent in the tuple
constexpr int TYPE_IDX = 6; // index of parent in the tuple

// NB: if more fields are added, make sure this field is also bumped
constexpr int RFD_TUPLE_SIZE = 7; // number of RRefForkData fields in py::tuple
} // namespace

py::tuple toPyTuple(const RRefForkData& forkData) {
  return py::make_tuple(
      forkData.ownerId_,
      forkData.rrefId_.createdOn_,
      forkData.rrefId_.localId_,
      forkData.forkId_.createdOn_,
      forkData.forkId_.localId_,
      forkData.parent_,
      forkData.typeStr_);
}
RRefForkData fromPyTuple(const py::tuple& pyTuple) {
  TORCH_INTERNAL_ASSERT(
      pyTuple.size() == RFD_TUPLE_SIZE,
      "Pickled RRefForkData must contain 6 numbers.");
  worker_id_t ownerId = pyTuple[OWNER_IDX].cast<worker_id_t>();
  // const reference will extend the lifetime of the temporary variable
  const RRefId& rrefId = RRefId(
      pyTuple[RREFID_ON_IDX].cast<worker_id_t>(),
      pyTuple[RREFID_ID_IDX].cast<local_id_t>());
  const RRefId& forkId = RRefId(
      pyTuple[FORKID_ON_IDX].cast<worker_id_t>(),
      pyTuple[FORKID_ID_IDX].cast<local_id_t>());

  worker_id_t parent = pyTuple[PARENT_IDX].cast<worker_id_t>();
  const std::string& typeStr = pyTuple[TYPE_IDX].cast<std::string>();

  return RRefForkData(ownerId, rrefId, forkId, parent, typeStr);
}
} // namespace rrefforkdata

} // namespace rpc
} // namespace distributed
} // namespace torch
