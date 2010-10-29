//
// Copyright (C) 2010 Codership Oy <info@codership.com>
//


#include "write_set.hpp"
#include "serialization.hpp"

#include "gu_logger.hpp"

using namespace std;
using namespace gu;


ostream& galera::operator<<(ostream& os, const Query& q)
{
    return (os << string(q.get_query().begin(), q.get_query().end()));
}


size_t galera::serialize(const Query& q,
                         gu::byte_t*  buf,
                         size_t       buf_len,
                         size_t       offset)
{
    offset = serialize<uint32_t>(q.query_, buf, buf_len, offset);
    offset = serialize(static_cast<int64_t>(q.tstamp_), buf, buf_len, offset);
    offset = serialize(q.rnd_seed_, buf, buf_len, offset);
    return offset;
}


size_t galera::unserialize(const gu::byte_t* buf,
                           size_t            buf_len,
                           size_t            offset,
                           Query&            q)
{
    q.query_.clear();
    offset = unserialize<uint32_t>(buf, buf_len, offset, q.query_);
    int64_t tstamp;
    offset = unserialize(buf, buf_len, offset, tstamp);
    q.tstamp_ = static_cast<time_t>(tstamp);
    offset = unserialize(buf, buf_len, offset, q.rnd_seed_);
    return offset;
}


size_t galera::serial_size(const Query& q)
{
    return (serial_size<uint32_t>(q.query_)
            + serial_size(int64_t())
            + serial_size(uint32_t()));
}


size_t galera::serialize(const RowKey& row_key,
                         gu::byte_t*   buf,
                         size_t        buf_len,
                         size_t        offset)
{
    offset = serialize<uint16_t>(row_key.table_, row_key.table_len_,
                                 buf, buf_len, offset);
    offset = serialize<uint16_t>(row_key.key_, row_key.key_len_, buf,
                                 buf_len, offset);
    offset = serialize(row_key.action_, buf, buf_len, offset);
    return offset;
}


size_t galera::unserialize(const gu::byte_t* buf,
                           size_t            buf_len,
                           size_t            offset,
                           RowKey&           row_key)
{
    offset = unserialize<uint16_t>(buf, buf_len, offset, row_key.table_,
                                   row_key.table_len_);
    offset = unserialize<uint16_t>(buf, buf_len, offset, row_key.key_,
                                   row_key.key_len_);
    offset = unserialize(buf, buf_len, offset, row_key.action_);
    return offset;
}


size_t galera::serial_size(const RowKey& row_key)
{
    return (serial_size<uint16_t>(row_key.table_, row_key.table_len_)
            + serial_size<uint16_t>(row_key.key_, row_key.key_len_)
            + serial_size(row_key.action_));
}


size_t galera::serialize(const WriteSet& ws,
                         gu::byte_t*     buf,
                         size_t          buf_len,
                         size_t          offset)
{
    uint32_t hdr(ws.level_ & 0xff);
    offset = serialize(hdr, buf, buf_len, offset);
    offset = serialize<QuerySequence::const_iterator, uint32_t>(
        ws.queries_.begin(), ws.queries_.end(), buf, buf_len, offset);
    offset = serialize<uint32_t>(
        &ws.keys_[0], ws.keys_.size(), buf, buf_len, offset);
    offset = serialize<uint32_t>(ws.data_, buf, buf_len, offset);
    return offset;
}


size_t galera::unserialize(const gu::byte_t* buf,
                           size_t            buf_len,
                           size_t            offset,
                           WriteSet&         ws)
{
    uint32_t hdr;
    offset = unserialize(buf, buf_len, offset, hdr);

    ws.level_ = static_cast<WriteSet::Level>(hdr & 0xff);
    ws.queries_.clear();
    offset = unserialize<Query, uint32_t>(
        buf, buf_len, offset, back_inserter(ws.queries_));
    ws.keys_.clear();
    offset = unserialize<uint32_t>(
        buf, buf_len, offset, ws.keys_);
    offset = unserialize<uint32_t>(buf, buf_len, offset, ws.data_);

    return offset;
}


size_t galera::serial_size(const WriteSet& ws)
{
    return (serial_size(uint32_t())
            + serial_size<QuerySequence::const_iterator, uint32_t>(
                ws.queries_.begin(), ws.queries_.end())
            + serial_size<uint32_t>(
                &ws.keys_[0], ws.keys_.size())
            + serial_size<uint32_t>(ws.data_));
}

void galera::WriteSet::append_row_key(const void* table,
                                      size_t      table_len,
                                      const void* key,
                                      size_t      key_len,
                                      int         action)
{
    RowKey rk(table, table_len, key, key_len, action);
    const size_t hash(rk.get_hash());

    pair<KeyRefMap::const_iterator, KeyRefMap::const_iterator>
        range(key_refs_.equal_range(hash));

    for (KeyRefMap::const_iterator i = range.first; i != range.second; ++i)
    {
        RowKey cmp;

        (void)galera::unserialize(&keys_[0], keys_.size(), i->second, cmp);

        if (rk == cmp) return;
    }

    size_t rk_size(serial_size(rk));
    size_t offset(keys_.size());
    keys_.resize(offset + rk_size);
    (void)galera::serialize(rk, &keys_[0], keys_.size(), offset);
    (void)key_refs_.insert(make_pair(hash, offset));
}


void galera::WriteSet::get_keys(RowKeySequence& s) const
{
    size_t offset(0);
    while (offset < keys_.size())
    {
        RowKey rk;
        if ((offset = unserialize(&keys_[0], keys_.size(), offset, rk)) == 0)
        {
            gu_throw_fatal << "failed to unserialize row key";
        }
        s.push_back(rk);
    }
    assert(offset == keys_.size());
}
