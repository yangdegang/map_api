#include <glog/logging.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/gzip_stream.h>
#include <google/protobuf/io/zero_copy_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

#include <map-api/chunk-manager.h>
#include <map-api/cr-table.h>
#include <map-api/cru-table.h>
#include <map-api/proto-table-file-io.h>
#include <map-api/transaction.h>

namespace map_api {
ProtoTableFileIO::ProtoTableFileIO(const std::string& filename,
                                   map_api::NetTable* table)
    : file_name_(filename), table_(CHECK_NOTNULL(table)) {
  zip_options_.format = kOutFormat;
  zip_options_.buffer_size = kZipBufferSize;
  zip_options_.compression_level = kZipCompressionLevel;

  file_.open(filename, kDefaultOpenmode);
  if (!file_.is_open()) {
    file_.open(filename, std::fstream::binary | std::fstream::in |
                             std::fstream::out | std::fstream::trunc);
  }
  CHECK(file_.is_open()) << "Couldn't open file " << filename;
}

ProtoTableFileIO::~ProtoTableFileIO() {}

void ProtoTableFileIO::truncFile() {
  file_.close();
  file_.open(file_name_, kDefaultOpenmode | std::ofstream::trunc);
}

bool ProtoTableFileIO::storeTableContents(const map_api::LogicalTime& time) {
  map_api::Transaction transaction(time);
  map_api::CRTable::RevisionMap revisions =
      transaction.dumpActiveChunks(table_);
  return storeTableContents(revisions);
}
bool ProtoTableFileIO::storeTableContents(
    const map_api::CRTable::RevisionMap& revisions) {
  CHECK(file_.is_open());

  for (const std::pair<Id, std::shared_ptr<Revision> >& pair : revisions) {
    CHECK(pair.second != nullptr);
    const Revision& revision = *pair.second;

    RevisionStamp current_item_stamp;
    current_item_stamp.first = revision.getId();
    CHECK_EQ(current_item_stamp.first, pair.first);

    current_item_stamp.second = revision.getModificationTime();

    // Format:
    // Store number of messages.
    // For each message store:
    //  -> size
    //  -> bytes from protobuf

    // Look up if we already stored this item, if we have this revision skip.
    bool already_stored = already_stored_items_.count(current_item_stamp) > 0;
    if (!already_stored) {
      // Moving read to the beginning of the file.
      file_.clear();
      file_.seekg(0);
      if (file_.peek() == std::char_traits<char>::eof()) {
        file_.clear();
        file_.seekp(0);
        google::protobuf::io::OstreamOutputStream raw_out(&file_);
        google::protobuf::io::GzipOutputStream gzip_out(&raw_out, zip_options_);
        google::protobuf::io::CodedOutputStream coded_out(&gzip_out);
        coded_out.WriteLittleEndian32(1);
      } else {
        file_.clear();
        file_.seekg(0);

        // Only creating these once we know the file isn't empty.
        google::protobuf::io::IstreamInputStream raw_in(&file_);
        google::protobuf::io::GzipInputStream gzip_in(&raw_in, kInFormat);
        google::protobuf::io::CodedInputStream coded_in(&gzip_in);
        uint32_t message_count;
        coded_in.ReadLittleEndian32(&message_count);

        ++message_count;

        file_.clear();
        file_.seekp(0);
        google::protobuf::io::OstreamOutputStream raw_out(&file_);
        google::protobuf::io::GzipOutputStream gzip_out(&raw_out, zip_options_);
        google::protobuf::io::CodedOutputStream coded_out(&gzip_out);
        coded_out.WriteLittleEndian32(message_count);
      }

      // Go to end of file and write message size and then the message.
      file_.clear();
      file_.seekp(0, std::ios_base::end);

      google::protobuf::io::OstreamOutputStream raw_out(&file_);
      google::protobuf::io::GzipOutputStream gzip_out(&raw_out, zip_options_);
      google::protobuf::io::CodedOutputStream coded_out(&gzip_out);

      coded_out.WriteVarint32(revision.byteSize());
      revision.underlyingRevision().SerializeToCodedStream(&coded_out);
      already_stored_items_.insert(current_item_stamp);
    }
  }
  return true;
}

bool ProtoTableFileIO::restoreTableContents() {
  Transaction transaction(LogicalTime::sample());
  restoreTableContents(&transaction);
  bool ok = transaction.commit();
  LOG_IF(WARNING, !ok) << "Transaction commit failed to load data";
  return ok;
}

bool ProtoTableFileIO::restoreTableContents(map_api::Transaction* transaction) {
  CHECK_NOTNULL(transaction);
  CHECK(file_.is_open());

  file_.clear();
  file_.seekg(0, std::ios::end);
  std::istream::pos_type file_size = file_.tellg();

  if (file_size == 0) {
    LOG(ERROR) << "Got file of size: " << file_size;
    return false;
  }

  file_.clear();
  file_.seekg(0, std::ios::beg);
  google::protobuf::io::IstreamInputStream raw_in(&file_);
  google::protobuf::io::GzipInputStream gzip_in(&raw_in, kInFormat);
  google::protobuf::io::CodedInputStream coded_in(&gzip_in);

  int kApproxMessageSizeAfterUncompression = file_size * 10;
  coded_in.SetTotalBytesLimit(kApproxMessageSizeAfterUncompression,
                              kApproxMessageSizeAfterUncompression);

  // Format:
  // Store number of messages.
  // For each message store:
  //  -> size
  //  -> bytes from protobuf

  uint32_t message_count;
  coded_in.ReadLittleEndian32(&message_count);

  if (message_count == 0) {
    LOG(ERROR) << "No messages in file.";
    return false;
  }

  std::unordered_map<Id, Chunk*> existing_chunks;

  for (size_t i = 0; i < message_count; ++i) {
    uint32_t msg_size;
    if (!coded_in.ReadVarint32(&msg_size)) {
      LOG(ERROR) << "Could not read message size."
                 << " while reading message " << i + 1 << " of "
                 << message_count << ".";
      return false;
    }
    if (msg_size == 0) {
      LOG(ERROR) << "Could not read message: size=0."
                 << " while reading message " << i + 1 << " of "
                 << message_count << ".";
      return false;
    }

    std::string input_string;
    if (!coded_in.ReadString(&input_string, msg_size)) {
      LOG(ERROR) << "Could not read message data"
                 << " while reading message " << i + 1 << " of "
                 << message_count << ".";
      return false;
    }

    std::shared_ptr<proto::Revision> proto_revision(new proto::Revision);
    std::shared_ptr<Revision> revision(new Revision(proto_revision));
    revision->parse(input_string);

    Id chunk_id = revision->getChunkId();
    Chunk* chunk = nullptr;
    std::unordered_map<Id, Chunk*>::iterator it =
        existing_chunks.find(chunk_id);
    if (it == existing_chunks.end()) {
      chunk = table_->newChunk(chunk_id);
      existing_chunks.insert(std::make_pair(chunk_id, chunk));
    } else {
      chunk = it->second;
    }
    CHECK_NOTNULL(chunk);

    transaction->insert(table_, chunk, revision);
  }
  return true;
}

}  // namespace map_api
