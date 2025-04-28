#include "engine/assignment.h"
#include "engine/rdma/rdma_assignment.h"
#include "engine/rdma/rdma_config.h"
#include "engine/rdma/rdma_transport.h"

#include "utils/json.hpp"
#include "utils/logging.h"
#include "utils/utils.h"

#include <cstdint>
#include <memory>
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

using json = nlohmann::json;

namespace py = pybind11;

PYBIND11_MODULE(_slime_c, m)
{
    py::enum_<slime::OpCode>(m, "OpCode")
        .value("READ", slime::OpCode::READ)
        .value("SEND", slime::OpCode::SEND)
        .value("RECV", slime::OpCode::RECV);
    py::class_<slime::Assignment>(m, "Assignment").def(py::init<std::string, uint64_t, uint64_t, uint64_t>());
    py::class_<slime::RDMASchedulerAssignment, std::shared_ptr<slime::RDMASchedulerAssignment>>(m, "RDMASchedulerAssignment")
        .def("wait", &slime::RDMASchedulerAssignment::wait, py::call_guard<py::gil_scoped_release>());
    py::class_<slime::RDMAContext>(m, "rdma_context")
        .def(py::init<>())
        .def("init_rdma_context", &slime::RDMAContext::init)
        .def("register_memory_region", &slime::RDMAContext::register_memory_region)
        .def("register_remote_memory_region",
             [](slime::RDMAContext& self, std::string mr_info) {
                 json json_info = json::parse(mr_info);
                 for (auto& item : json_info["mr_info"].items())
                     self.register_remote_memory_region(item.key(), item.value());
             })
        .def("local_info", [](slime::RDMAContext& self) { return self.local_info().dump(); })
        .def("connect",
             [](slime::RDMAContext& self, std::string remote_info) {
                 json            json_info        = json::parse(remote_info);
                 slime::RDMAInfo remote_rdma_info = slime::RDMAInfo(json_info["rdma_info"]);
                 self.connect_to(remote_rdma_info);
                 for (auto& mr_info : json_info["mr_info"].items())
                     self.register_remote_memory_region(mr_info.key(), mr_info.value());
             })
        .def("launch_future", &slime::RDMAContext::launch_future)
        .def("stop_future", &slime::RDMAContext::stop_future)
        .def(
            "submit",
            [](slime::RDMAContext& self, slime::OpCode opcode, slime::AssignmentBatch batch) {
                slime::RDMAAssignment* rdma_assignment = new slime::RDMAAssignment(opcode, batch);
                self.submit(rdma_assignment);
                return std::make_shared<slime::RDMASchedulerAssignment>(slime::RDMAAssignmentPtrBatch{rdma_assignment});
            },
            py::call_guard<py::gil_scoped_release>());

    m.def("available_nic", &slime::available_nic);

}
