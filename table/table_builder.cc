// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/table_builder.h"

#include <assert.h>

#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "table/block_builder.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {

struct TableBuilder::Rep {
  Rep(const Options& opt, WritableFile* f)
      : options(opt),
        index_block_options(opt),
        file(f),
        offset(0),
        data_block(&options),
        index_block(&index_block_options),
        num_entries(0),
        closed(false),
        filter_block(opt.filter_policy == nullptr
                         ? nullptr
                         : new FilterBlockBuilder(opt.filter_policy)),
        pending_index_entry(false) {
    index_block_options.block_restart_interval = 1;
  }

  Options options;
  Options index_block_options;
  WritableFile* file;   //要生成的.sst文件 
  uint64_t offset;      //累加每个Data Block的偏移量
  Status status;
  BlockBuilder data_block;
  BlockBuilder index_block;
  std::string last_key; //上一个插入的key值，新插入的key必须比它大，保证.sst文件中的key是从小到大排列的
  int64_t num_entries;  //.sst文件中存储的所有记录总数。
  bool closed;  // Either Finish() or Abandon() has been called.
  FilterBlockBuilder* filter_block;

  // We do not emit the index entry for a block until we have seen the
  // first key for the next data block.  This allows us to use shorter
  // keys in the index block.  For example, consider a block boundary
  // between the keys "the quick brown fox" and "the who".  We can use
  // "the r" as the key for the index block entry since it is >= all
  // entries in the first block and < all entries in subsequent
  // blocks.
  //
  // Invariant: r->pending_index_entry is true only if data_block is empty.
  bool pending_index_entry;         // 当一个Data Block被写入到.sst文件时，为true
  BlockHandle pending_handle;       // Handle to add to index block

  std::string compressed_output;    // Data Block的block_data字段压缩后的结果
};

TableBuilder::TableBuilder(const Options& options, WritableFile* file)
    : rep_(new Rep(options, file)) {
  if (rep_->filter_block != nullptr) {
    rep_->filter_block->StartBlock(0);
  }
}

TableBuilder::~TableBuilder() {
  assert(rep_->closed);  // Catch errors where caller forgot to call Finish()
  delete rep_->filter_block;
  delete rep_;
}

Status TableBuilder::ChangeOptions(const Options& options) {
  // Note: if more fields are added to Options, update
  // this function to catch changes that should not be allowed to
  // change in the middle of building a Table.
  if (options.comparator != rep_->options.comparator) {
    return Status::InvalidArgument("changing comparator while building table");
  }

  // Note that any live BlockBuilders point to rep_->options and therefore
  // will automatically pick up the updated options.
  rep_->options = options;
  rep_->index_block_options = options;
  rep_->index_block_options.block_restart_interval = 1;
  return Status::OK();
}

void TableBuilder::Add(const Slice& key, const Slice& value) {
  Rep* r = rep_;
  assert(!r->closed);
  if (!ok()) return;
  if (r->num_entries > 0) {
    assert(r->options.comparator->Compare(key, Slice(r->last_key)) > 0);
  }

  if (r->pending_index_entry) {                                         // 构建index_block（保存每个 data_block 的 last_key 及其在 sstable 文件中的索引）
    assert(r->data_block.empty());                                      // 说明到了新的一个Data Block
    
    r->options.comparator->FindShortestSeparator(&r->last_key, key);    // 计算出上一个 block 的 end-key
    std::string handle_encoding;
    r->pending_handle.EncodeTo(&handle_encoding);
    r->index_block.Add(r->last_key, Slice(handle_encoding));            // 连同 BlockHandle 添加到 index_block 中
    r->pending_index_entry = false;
  }

  if (r->filter_block != nullptr) {
    r->filter_block->AddKey(key);                                       // 构建过滤器
  }

  r->last_key.assign(key.data(), key.size());
  r->num_entries++;
  r->data_block.Add(key, value);                                        // 将 key/value 加入当前 data_block

  const size_t estimated_block_size = r->data_block.CurrentSizeEstimate();
  if (estimated_block_size >= r->options.block_size) {                  // 数据块(Data Block)大小已达上限，写入磁盘
    Flush();                                                            // 注意这里以data block为单位刷盘，而不是整个sst
  }
}

void TableBuilder::Flush() {
  Rep* r = rep_;
  assert(!r->closed);
  if (!ok()) return;
  if (r->data_block.empty()) return;
  assert(!r->pending_index_entry);
  WriteBlock(&r->data_block, &r->pending_handle);                       // 将 data_block 写入磁盘
  if (ok()) {
    r->pending_index_entry = true;
    r->status = r->file->Flush();
  }
  if (r->filter_block != nullptr) {
    r->filter_block->StartBlock(r->offset);
  }
}

void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
  // File format contains a sequence of blocks where each block has:
  //    block_data: uint8[n]
  //    type: uint8
  //    crc: uint32
  assert(ok());
  Rep* r = rep_;
  Slice raw = block->Finish();

  Slice block_contents;
  CompressionType type = r->options.compression;
  // TODO(postrelease): Support more compression options: zlib?a
  switch (type) {                                       // 对 block 的数据做可选的压缩（snappy）
    case kNoCompression:
      block_contents = raw;
      break;
    case kSnappyCompression: {                          //  采用Snappy压缩，Snappy是谷歌开源的压缩库
      std::string* compressed = &r->compressed_output;
      if (port::Snappy_Compress(raw.data(), raw.size(), compressed) && 
          compressed->size() < raw.size() - (raw.size() / 8u)) {
        block_contents = *compressed;
      } else {
        // Snappy not supported, or compressed(压缩比) less than 12.5%, so just store uncompressed form
        block_contents = raw;
        type = kNoCompression;
      }
      break;
    }
  }
  WriteRawBlock(block_contents, type, handle);          // append 到 sstable 文件
  r->compressed_output.clear();
  block->Reset();
}

void TableBuilder::WriteRawBlock(const Slice& block_contents, CompressionType type, BlockHandle* handle) {
  Rep* r = rep_;
  handle->set_offset(r->offset);
  handle->set_size(block_contents.size());                                      // 记录该 block 的 BlockHandle
  r->status = r->file->Append(block_contents);
  if (r->status.ok()) {
    char trailer[kBlockTrailerSize];
    trailer[0] = type;

    uint32_t crc = crc32c::Value(block_contents.data(), block_contents.size()); // 为block_contents添加校验
    crc = crc32c::Extend(crc, trailer, 1);                                      // Extend crc to cover block type
    EncodeFixed32(trailer + 1, crc32c::Mask(crc));                              // 将校验码拷贝到trailer的后四个字节
    r->status = r->file->Append(Slice(trailer, kBlockTrailerSize));             // 添加该 block 的 trailer（type/crc），一个完整的Block Data诞生
    if (r->status.ok()) {
      r->offset += block_contents.size() + kBlockTrailerSize;                   // 偏移应该包括数据内容+压缩类型+校验码的大小
    }
  }
}

Status TableBuilder::status() const { return rep_->status; }

// sstable 结构
// 1.data_block
// 2.filter_block
// 3.meta_index_block
// 4.index_block
// 5.footer
Status TableBuilder::Finish() { // 在 sstable 完成时（dump memtable 完成或者达到 kTargetFileSize）做的处理
  Rep* r = rep_;
  Flush(); // 因为调用Finish的时候，block_data不一定大于等于block_size，所以要调用Flush，将这部分block_data写入到磁盘
  assert(!r->closed);
  r->closed = true;

  BlockHandle filter_block_handle, metaindex_block_handle, index_block_handle;

  // Write filter block
  if (ok() && r->filter_block != nullptr) {
    WriteRawBlock(r->filter_block->Finish(), kNoCompression, &filter_block_handle);
  }

  // Write metaindex block
  if (ok()) {
    BlockBuilder meta_index_block(&r->options);
    if (r->filter_block != nullptr) {
      // Add mapping from "filter.Name" to location of filter data
      std::string key = "filter.";
      key.append(r->options.filter_policy->Name());
      std::string handle_encoding;
      filter_block_handle.EncodeTo(&handle_encoding);
      meta_index_block.Add(key, handle_encoding);
    }

    // TODO(postrelease): Add stats and other meta blocks
    WriteBlock(&meta_index_block, &metaindex_block_handle);     // 将 meta_index_block 写入磁盘
  }

  // Write index block
  if (ok()) {
    if (r->pending_index_entry) {
      r->options.comparator->FindShortSuccessor(&r->last_key);  // 计算最后一个 block 的 end-key
      std::string handle_encoding;
      r->pending_handle.EncodeTo(&handle_encoding);
      r->index_block.Add(r->last_key, Slice(handle_encoding));  // 连同其 BlockHandle 添加到 index_block 中
      r->pending_index_entry = false;
    }
    WriteBlock(&r->index_block, &index_block_handle);           // 将 index_block 写入磁盘
  }

  // Write footer
  if (ok()) {
    Footer footer;                                              // 构造 footer
    footer.set_metaindex_handle(metaindex_block_handle);        // footer 里包含两个 handle：metaindex_block_handle 和 index_block_handle
    footer.set_index_handle(index_block_handle);
    std::string footer_encoding;
    footer.EncodeTo(&footer_encoding);
    r->status = r->file->Append(footer_encoding);               // footer 作为最后部分写入 sstable
    if (r->status.ok()) {
      r->offset += footer_encoding.size();
    }
  }
  return r->status;
}

void TableBuilder::Abandon() {
  Rep* r = rep_;
  assert(!r->closed);
  r->closed = true;
}

uint64_t TableBuilder::NumEntries() const { return rep_->num_entries; }

uint64_t TableBuilder::FileSize() const { return rep_->offset; }

}  // namespace leveldb
