#include <gtest/gtest.h>
#include <filesystem>
#include "../../deeplog/deeplog.hpp"
#include "../../deeplog/actions/protocol_action.hpp"
#include "../../deeplog/actions/metadata_action.hpp"
#include "../../deeplog/last_checkpoint.hpp"
#include "../../deeplog/snapshot.hpp"
#include "../../deeplog/metadata_snapshot.hpp"
#include "../../deeplog/optimistic_transaction.hpp"
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include <arrow/io/api.h>
#include <arrow/compute/api.h>
#include <parquet/api/reader.h>
#include <parquet/arrow/reader.h>

class DeeplogTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (std::filesystem::exists(test_dir)) {
            std::filesystem::remove_all(test_dir);
        }
    }

    void TearDown() override {
        if (std::filesystem::exists(test_dir)) {
            std::filesystem::remove_all(test_dir);
        }
    }

    std::set<std::string> list_log_files(const std::string &branch_id) {
        auto files = std::set < std::string > ();
        std::filesystem::path dir_path = {test_dir + "/_deeplake_log/" + branch_id + "/"};
        for (const auto &entry: std::filesystem::directory_iterator(dir_path)) {
            files.insert(entry.path().string().substr((test_dir + "/_deeplake_log/" + branch_id + "/").size()));
        }

        return files;
    }

    std::string test_dir = "tmp/test";
};


TEST_F(DeeplogTest, create) {
    auto log = deeplog::deeplog::create(test_dir, 4);

    ASSERT_TRUE(std::filesystem::exists({test_dir + "/_deeplake_log/"}));
    ASSERT_EQ(std::set < std::string > {"00000000000000000001.json"}, list_log_files(deeplog::META_BRANCH_ID));

    std::ifstream ifs(test_dir + "/_deeplake_log/_meta/00000000000000000001.json");
    std::ostringstream json_string_stream;
    json_string_stream << ifs.rdbuf();
    auto json_string = json_string_stream.str();

    EXPECT_FALSE(json_string.starts_with("["));
    EXPECT_TRUE(json_string.find("protocol") != std::string::npos);
    EXPECT_TRUE(json_string.find("metadata") != std::string::npos);
    EXPECT_TRUE(json_string.find("branch") != std::string::npos);

    auto meta_snapshot = deeplog::metadata_snapshot(log);

    EXPECT_EQ(1, meta_snapshot.branches().size());
    EXPECT_EQ("main", meta_snapshot.branches().at(0)->name);
    EXPECT_EQ(4, meta_snapshot.protocol()->min_reader_version);
    EXPECT_EQ(4, meta_snapshot.protocol()->min_writer_version);

    EXPECT_NE("", meta_snapshot.metadata()->id);
    EXPECT_NE(0, meta_snapshot.metadata()->created_time);
    EXPECT_FALSE(meta_snapshot.metadata()->name.has_value());
    EXPECT_FALSE(meta_snapshot.metadata()->description.has_value());

    auto snapshot = deeplog::snapshot("main", 0, log);
    const auto files = snapshot.data_files();
    EXPECT_EQ(0, files.size());

    EXPECT_THROW(auto ignore = deeplog::deeplog::create(test_dir, 4), std::runtime_error) << "Should not be able to create log twice";
}

TEST_F(DeeplogTest, open) {
    auto ignore = deeplog::deeplog::create(test_dir, 4);

    auto log = deeplog::deeplog::open(test_dir);

    EXPECT_EQ(1, log->version(deeplog::META_BRANCH_ID));
}

TEST_F(DeeplogTest, version) {
    auto log = deeplog::deeplog::create(test_dir, 4);
    EXPECT_EQ(1, log->version(deeplog::META_BRANCH_ID));

    EXPECT_EQ(0, log->version(deeplog::metadata_snapshot(log).find_branch("main")->id));
}

TEST_F(DeeplogTest, find_branch) {
    auto log = deeplog::deeplog::create(test_dir, 4);

    auto main_branch = deeplog::metadata_snapshot(log).find_branch("main");
    EXPECT_EQ("main", main_branch->name);
    EXPECT_NE("", main_branch->id);

}


TEST_F(DeeplogTest, commit_protocol) {
    auto log = deeplog::deeplog::create(test_dir, 4);

    auto action = deeplog::protocol_action(5, 6);
    log->commit(deeplog::META_BRANCH_ID, 1, {std::make_shared<deeplog::protocol_action>(action)});

    EXPECT_EQ((std::set < std::string > {"00000000000000000001.json", "00000000000000000002.json"}), list_log_files(deeplog::META_BRANCH_ID));
    std::ifstream ifs(test_dir + "/_deeplake_log/" + deeplog::META_BRANCH_ID + "/00000000000000000001.json");
    std::ostringstream json_string_stream;
    json_string_stream << ifs.rdbuf();
    auto json_string = json_string_stream.str();

    EXPECT_NE(json_string.find("protocol"), std::string::npos);

    EXPECT_EQ(5, deeplog::metadata_snapshot(log).protocol()->min_reader_version);
    EXPECT_EQ(6, deeplog::metadata_snapshot(log).protocol()->min_writer_version);
}

TEST_F(DeeplogTest, commit_metadata) {
    auto log = deeplog::deeplog::create(test_dir, 4);

    auto original_metadata = deeplog::metadata_snapshot(log).metadata();
    auto action = deeplog::metadata_action(original_metadata->id, "new name", "new desc", original_metadata->created_time);
    log->commit(deeplog::META_BRANCH_ID, log->version(deeplog::META_BRANCH_ID), {std::make_shared<deeplog::metadata_action>(action)});

    EXPECT_EQ((std::set < std::string > {"00000000000000000001.json", "00000000000000000002.json"}), list_log_files(deeplog::META_BRANCH_ID));
    std::ifstream ifs(test_dir + "/_deeplake_log/" + deeplog::META_BRANCH_ID + "/00000000000000000002.json");
    std::ostringstream json_string_stream;
    json_string_stream << ifs.rdbuf();
    auto json_string = json_string_stream.str();

    EXPECT_NE(json_string.find("metadata"), std::string::npos);

    auto new_metadata = deeplog::metadata_snapshot(log).metadata();
    EXPECT_EQ(original_metadata->id, new_metadata->id);
    EXPECT_EQ(original_metadata->created_time, new_metadata->created_time);
    EXPECT_EQ("new name", new_metadata->name);
    EXPECT_EQ("new desc", new_metadata->description);
}

TEST_F(DeeplogTest, commit_add_file) {
    auto log = deeplog::deeplog::create(test_dir, 4);
    auto main_id = deeplog::metadata_snapshot(log).find_branch("main")->id;

    auto action = deeplog::add_file_action("my/path", "chunk", 3, 45, true, 3);
    log->commit(main_id, log->version(main_id), {std::make_shared<deeplog::add_file_action>(action)});

    EXPECT_EQ((std::set < std::string > {"00000000000000000001.json"}), list_log_files(main_id));
    std::ifstream ifs(test_dir + "/_deeplake_log/" + main_id + "/00000000000000000001.json");
    std::ostringstream json_string_stream;
    json_string_stream << ifs.rdbuf();
    auto json_string = json_string_stream.str();

    EXPECT_NE(json_string.find("add"), std::string::npos);

    auto files = deeplog::snapshot(main_id, 1, log).data_files();

    EXPECT_EQ(1, files.size());
    EXPECT_EQ("my/path", files.at(0)->path);
    EXPECT_EQ(3, files.at(0)->size);
    EXPECT_EQ(45, files.at(0)->modification_time);
}

TEST_F(DeeplogTest, commit_create_branch) {
    auto log = deeplog::deeplog::create(test_dir, 4);

    auto action = deeplog::create_branch_action("123", "branch1", deeplog::META_BRANCH_ID, 0);
    log->commit(deeplog::META_BRANCH_ID, log->version(deeplog::META_BRANCH_ID), {std::make_shared<deeplog::create_branch_action>(action)});

    EXPECT_EQ((std::set < std::string > {"00000000000000000001.json", "00000000000000000002.json"}), list_log_files(deeplog::META_BRANCH_ID));
    std::ifstream ifs(test_dir + "/_deeplake_log/" + deeplog::META_BRANCH_ID + "/00000000000000000002.json");
    std::ostringstream json_string_stream;
    json_string_stream << ifs.rdbuf();
    auto json_string = json_string_stream.str();

    EXPECT_NE(json_string.find("branch"), std::string::npos);

    auto branches = deeplog::metadata_snapshot(log).branches();

    EXPECT_EQ(2, branches.size());
    EXPECT_NE("", (branches).at(0)->id);
    EXPECT_EQ("main", (branches).at(0)->name);

    EXPECT_EQ("123", (branches).at(1)->id);
    EXPECT_EQ("branch1", (branches).at(1)->name);
}

TEST_F(DeeplogTest, checkpoint) {
    auto log = deeplog::deeplog::create(test_dir, 4);

    auto main_id = deeplog::metadata_snapshot(log).find_branch("main")->id;
    auto original_metadata = deeplog::metadata_snapshot(log).metadata();
    for (int i = 0; i <= 3; ++i) {
        auto action = deeplog::metadata_action(original_metadata->id, "name " + std::to_string(i), "desc " + std::to_string(i), original_metadata->created_time);
        log->commit(deeplog::META_BRANCH_ID, log->version(deeplog::META_BRANCH_ID), {std::make_shared<deeplog::metadata_action>(action)});
    }

    for (int i = 0; i < 4; ++i) {
        auto action = deeplog::add_file_action("my/path" + std::to_string(i), "chunk", 3, 45, true, 10);
        log->commit(main_id, log->version(main_id), {std::make_shared<deeplog::add_file_action>(action)});
    }

    EXPECT_EQ(5, log->version(deeplog::META_BRANCH_ID));
    EXPECT_EQ(4, log->version(main_id));

    EXPECT_EQ(5, list_log_files(deeplog::META_BRANCH_ID).size());
    EXPECT_EQ(4, list_log_files(main_id).size());

    auto new_metadata = deeplog::metadata_snapshot(log).metadata();
    EXPECT_EQ(original_metadata->id, new_metadata->id);
    EXPECT_EQ(original_metadata->created_time, new_metadata->created_time);
    EXPECT_EQ("name 3", new_metadata->name);
    EXPECT_EQ("desc 3", new_metadata->description);

    log->checkpoint(deeplog::META_BRANCH_ID);
    EXPECT_TRUE(list_log_files(deeplog::META_BRANCH_ID).contains("00000000000000000005.checkpoint.parquet"));
    EXPECT_TRUE(list_log_files(deeplog::META_BRANCH_ID).contains("_last_checkpoint.json"));

    std::ifstream ifs(test_dir + "/_deeplake_log/" + deeplog::META_BRANCH_ID + "/_last_checkpoint.json");
    deeplog::last_checkpoint checkpoint_content = nlohmann::json::parse(ifs).template get<deeplog::last_checkpoint>();
    EXPECT_EQ(5, checkpoint_content.version);


    //delete json files so loads after checkpoint doesn't use it
    for (auto file: list_log_files(deeplog::META_BRANCH_ID)) {
        if (file != "_last_checkpoint.json" && file.ends_with(".json")) {
            std::filesystem::remove(test_dir + "/_deeplake_log/" + deeplog::META_BRANCH_ID + "/" + file);
        }
    }
    ASSERT_FALSE(list_log_files(deeplog::META_BRANCH_ID).contains("00000000000000000001.json"));

    auto new_log = deeplog::deeplog::open(test_dir);
    new_metadata = deeplog::metadata_snapshot(new_log).metadata();
    EXPECT_EQ(5, new_log->version(deeplog::META_BRANCH_ID));
    EXPECT_EQ(original_metadata->id, new_metadata->id);
    EXPECT_EQ("name 3", new_metadata->name);
}

TEST_F(DeeplogTest, checkpoint_collapses_actions) {
    auto log = deeplog::deeplog::create(test_dir, 4);

    auto original_metadata = deeplog::metadata_snapshot(log).metadata();

    log->commit(deeplog::META_BRANCH_ID, log->version(deeplog::META_BRANCH_ID), {std::make_shared<deeplog::metadata_action>(deeplog::metadata_action(original_metadata->id, "first name", "first desc", original_metadata->created_time))});
    log->commit(deeplog::META_BRANCH_ID, log->version(deeplog::META_BRANCH_ID), {std::make_shared<deeplog::metadata_action>(deeplog::metadata_action(original_metadata->id, "final name", "final desc", original_metadata->created_time))});

    log->checkpoint(deeplog::META_BRANCH_ID);

    ASSERT_TRUE(list_log_files(deeplog::META_BRANCH_ID).contains("00000000000000000003.checkpoint.parquet"));

    auto checkpoint_file = arrow::io::ReadableFile::Open(test_dir + "/_deeplake_log/" + deeplog::META_BRANCH_ID + "/00000000000000000003.checkpoint.parquet").ValueOrDie();
    std::unique_ptr<parquet::arrow::FileReader> arrow_reader;
    EXPECT_TRUE(parquet::arrow::OpenFile(checkpoint_file, arrow::default_memory_pool(), &arrow_reader).ok());

    std::shared_ptr<arrow::Table> table;
    EXPECT_TRUE(arrow_reader->ReadTable(&table).ok());

    auto metadata_values = arrow::compute::DropNull(table->GetColumnByName("metadata")).ValueOrDie();
    EXPECT_EQ(1, metadata_values.chunked_array()->length());
    EXPECT_EQ("final name", std::dynamic_pointer_cast<arrow::StructScalar>(metadata_values.chunked_array()->GetScalar(0).ValueOrDie())->field("name").ValueOrDie()->ToString());
    EXPECT_EQ("final desc", std::dynamic_pointer_cast<arrow::StructScalar>(metadata_values.chunked_array()->GetScalar(0).ValueOrDie())->field("description").ValueOrDie()->ToString());
}

TEST_F(DeeplogTest, manual) {
    auto log = deeplog::deeplog::create(test_dir, 4);
    auto metadata_snapshot = std::make_shared<deeplog::metadata_snapshot>(deeplog::metadata_snapshot(log));

    auto tx = deeplog::optimistic_transaction(metadata_snapshot);
    tx.add(std::make_shared<deeplog::create_tensor_action>(deeplog::create_tensor_action(
            "123",
            "tensor name",
            "text",
            "other text",
            55,
            false,
            false,
            false,
            std::nullopt,
            std::nullopt,
            {
                    {"link1", deeplog::tensor_link("123", true, "456")},
                    {"link2", deeplog::tensor_link("789", false, "101112")},
            },
            6243,
            {1, 2, 3},
            {4, 5, 6},
            std::nullopt,
            std::nullopt,
            true,
            "1.3.2")));

    tx.commit();
}

//TEST(IntTest, e2eTest) {
//    auto test_dir = "../test-ds";
////    auto log = deeplog::deeplog::create(test_dir, 4);
//    auto log = deeplog::deeplog::open(test_dir);
//
//    const auto &current_metadata = log->metadata();
//    std::cout << current_metadata.data->id << std::endl;
//
//    for (auto file : log->data_files(deeplog::MAIN_BRANCH_ID, std::nullopt).data) {
//        std::cout << file->path() << std::endl;
//    }
//
//    auto action = deeplog::metadata_action(current_metadata.data->id, "new name", "new desc", current_metadata.data->created_time);
//    log->commit(deeplog::MAIN_BRANCH_ID, current_metadata.version, {&action});
//
////    auto action = deeplog::add_file_action("path/to/file.txt", 15, deeplog::current_timestamp(), true);
////    log->commit(deeplog::MAIN_BRANCH_ID, 1, {&action});
//
////    log->checkpoint(deeplog::MAIN_BRANCH_ID);
//}
