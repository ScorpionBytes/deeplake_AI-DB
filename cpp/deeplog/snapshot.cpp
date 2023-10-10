#include "snapshot.hpp"
#include "metadata_snapshot.hpp"
#include <utility>

namespace deeplog {

    snapshot::snapshot(std::string branch_id, const std::shared_ptr<::deeplog::deeplog> &deeplog) :
            base_snapshot(branch_id, std::nullopt, deeplog) {}

    snapshot::snapshot(std::string branch_id, const unsigned long &version, const std::shared_ptr<::deeplog::deeplog> &deeplog) :
            base_snapshot(branch_id, version, deeplog) {}

    std::vector<std::shared_ptr<add_file_action>> snapshot::data_files() {
        return find_actions<add_file_action>();
    }

    std::vector<std::shared_ptr<create_commit_action>> snapshot::commits() {
        return find_actions<create_commit_action>();

    }

    std::vector<std::shared_ptr<create_tensor_action>> snapshot::tensors() {
        return find_actions<create_tensor_action>();

    }

}