
#include "evs_proto.hpp"
#include "evs_message2.hpp"
#include "evs_input_map2.hpp"

#include "gcomm/transport.hpp"

#include <stdexcept>
#include <algorithm>

using namespace std;
using namespace gcomm;
using namespace gcomm::evs;

static bool msg_from_previous_view(const list<pair<ViewId, Time> >& views, 
                                   const Message& msg)
{
    for (list<pair<ViewId, Time> >::const_iterator i = views.begin();
         i != views.end(); ++i)
    {
        if (msg.get_source_view_id() == i->first)
        {
            log_debug << "message " << msg 
                      << " from previous view " << i->first;
            return true;
        }
    }
    return false;
}


gcomm::evs::Proto::Proto(EventLoop* el_, 
                         Transport* t, 
                         const UUID& my_uuid_, 
                         Monitor* mon_) : 
    mon(mon_),
    tp(t),
    el(el_),
    collect_stats(true),
    hs_safe("0.0,0.0005,0.001,0.002,0.005,0.01,0.02,0.05,0.1,0.5,1.,5.,10.,30."),
    delivering(false),
    my_uuid(my_uuid_), 
    known(),
    self_i(),
    inactive_timeout(Time(5, 0)),
    inactive_check_period(Time(1, 0)),
    consensus_timeout(Time(1, 0)),
    resend_period(Time(1, 0)),
    send_join_period(Time(0, 300000)),
    timer(el_),
    current_view(View::V_TRANS, ViewId(my_uuid, 0)),
    previous_view(),
    previous_views(),
    im(),
    install_message(0),
    installing(false),
    fifo_seq(-1),
    last_sent(Seqno::max()),
    send_window(8), 
    output(),
    max_output_size(128),
    self_loopback(false),
    state(S_CLOSED),
    shift_to_rfcnt(0),
    ith(), cth(), consth(), resendth(), sjth()
{
    known.insert_checked(make_pair(my_uuid, Node()));
    self_i = known.begin();
    assert(NodeMap::get_value(self_i).get_operational() == true);
    
    im.insert_uuid(my_uuid);
    current_view.add_member(my_uuid, "");
    
    ith = new InactivityTimerHandler(*this);
    cth = new CleanupTimerHandler(*this);
    consth = new ConsensusTimerHandler(*this);
    resendth = new ResendTimerHandler(*this);
    sjth = new SendJoinTimerHandler(*this);
}


gcomm::evs::Proto::~Proto() 
{
    for (deque<pair<WriteBuf*, ProtoDownMeta> >::iterator i = output.begin(); i != output.end();
         ++i)
    {
        delete i->first;
    }
    output.clear();
    delete install_message;
    delete ith;
    delete cth;
    delete consth;
    delete resendth;
    delete sjth;
}

ostream& gcomm::evs::operator<<(ostream& os, const Proto& p)
{
    return (os << "evs::proto");
}


void gcomm::evs::Proto::check_inactive()
{
    bool has_inactive = false;
    for (NodeMap::iterator i = known.begin(); i != known.end(); ++i)
    {
        if (NodeMap::get_key(i) != get_uuid() &&
            NodeMap::get_value(i).get_operational() == true &&
            NodeMap::get_value(i).get_tstamp() + inactive_timeout < Time::now())
        {
            log_info << self_string() << " detected inactive node: " 
                     << NodeMap::get_key(i);
            
            i->second.set_operational(false);
            has_inactive = true;
        }
    }
    if (has_inactive == true && get_state() == S_OPERATIONAL)
    {
        shift_to(S_RECOVERY, true);
#if 0
        if (is_consensus() && is_representative(get_uuid()))
        {
            send_install();
        }
#endif
    }
}

void gcomm::evs::Proto::set_inactive(const UUID& uuid)
{
    NodeMap::iterator i;
    gu_trace(i = known.find_checked(uuid));
    log_debug << self_string() << " setting " << uuid << " inactive";
    NodeMap::get_value(i).set_tstamp(Time(0, 0));
}

void gcomm::evs::Proto::cleanup_unoperational()
{
    NodeMap::iterator i, i_next;
    for (i = known.begin(); i != known.end(); i = i_next) 
    {
        i_next = i, ++i_next;
        if (NodeMap::get_value(i).get_installed() == false)
        {
            log_debug << self_string() << " erasing " 
                      << NodeMap::get_key(i);
            known.erase(i);
        }
    }
}

void gcomm::evs::Proto::cleanup_views()
{
    Time now(Time::now());
    list<pair<ViewId, Time> >::iterator i = previous_views.begin();
    while (i != previous_views.end())
    {
        if (i->second + Time(300, 0) < now)
        {
            log_info << self_string() << " erasing view: " << i->first;
            previous_views.erase(i);
        }
        else
        {
            break;
        }
        i = previous_views.begin();
    }
}

size_t gcomm::evs::Proto::n_operational() const 
{
    NodeMap::const_iterator i;
    size_t ret = 0;
    for (i = known.begin(); i != known.end(); ++i) {
        if (i->second.get_operational())
            ret++;
    }
    return ret;
}

void gcomm::evs::Proto::deliver_reg_view()
{
    if (install_message == 0)
    {
        gcomm_throw_fatal
            << "Protocol error: no install message in deliver reg view";
    }
    
    if (previous_views.size() == 0) gcomm_throw_fatal << "Zero-size view";
    
    const View& prev_view (previous_view);
    View view (View::V_REG, install_message->get_source_view_id());
    
    for (NodeMap::iterator i = known.begin(); i != known.end(); ++i)
    {
        if (NodeMap::get_value(i).get_installed())
        {
            view.add_member(NodeMap::get_key(i), "");            
            if (prev_view.get_members().find(NodeMap::get_key(i)) ==
                prev_view.get_members().end())
            {
                view.add_joined(NodeMap::get_key(i), "");
            }
        }
        else if (NodeMap::get_value(i).get_installed() == false)
        {
            const MessageNodeList& instances = install_message->get_node_list();
            MessageNodeList::const_iterator inst_i;
            if ((inst_i = instances.find(NodeMap::get_key(i))) != instances.end())
            {
                if (MessageNodeList::get_value(inst_i).get_leaving())
                {
                    view.add_left(NodeMap::get_key(i), "");
                }
                else
                {
                    view.add_partitioned(NodeMap::get_key(i), "");
                }
            }
            assert(NodeMap::get_key(i) != get_uuid());
            NodeMap::get_value(i).set_operational(false);
        }
    }
    
    log_debug << view;
    ProtoUpMeta up_meta(&view);
    pass_up(0, 0, &up_meta);
}

void gcomm::evs::Proto::deliver_trans_view(bool local) 
{
    if (local == false && install_message == 0)
    {
        gcomm_throw_fatal
            << "Protocol error: no install message in deliver trans view";
    }
    
    log_debug << self_string();
    
    View view(View::V_TRANS, current_view.get_id());
    
    for (NodeMap::const_iterator i = known.begin(); i != known.end(); ++i)
    {
        const UUID& uuid = NodeMap::get_key(i);
        const Node& inst = NodeMap::get_value(i);
        
        if (inst.get_installed() == true && 
            current_view.get_members().find(uuid) != 
            current_view.get_members().end() &&
            (local == true ||
             inst.get_join_message()->get_source_view_id() == current_view.get_id()))
        {
            view.add_member(NodeMap::get_key(i), "");
        }
        else if (inst.get_installed() == false)
        {
            if (local == false)
            {
                const MessageNodeList& instances = install_message->get_node_list();
                MessageNodeList::const_iterator inst_i;
                if ((inst_i = instances.find(NodeMap::get_key(i))) != instances.end())
                {
                    if (MessageNodeList::get_value(inst_i).get_leaving())
                    {
                        view.add_left(NodeMap::get_key(i), "");
                    }
                    else
                    {
                        view.add_partitioned(NodeMap::get_key(i), "");
                    }
                }
            }
            else
            {
                // Just assume others have partitioned, it does not matter
                // for leaving node anyway and it is not guaranteed if
                // the others get the leave message, so it is not safe
                // to assume then as left.
                view.add_partitioned(NodeMap::get_key(i), "");
            }
        }
        else
        {
            // merging nodes, these won't be visible in trans view
        }
    }
    log_debug << view;
    gcomm_assert(view.get_members().find(get_uuid()) != view.get_members().end());
    ProtoUpMeta up_meta(&view);
    pass_up(0, 0, &up_meta);
}


void gcomm::evs::Proto::deliver_empty_view()
{
    View view(View::V_REG, ViewId());
    log_debug << view;
    ProtoUpMeta up_meta(&view);
    pass_up(0, 0, &up_meta);
}


void gcomm::evs::Proto::setall_installed(bool val)
{
    for (NodeMap::iterator i = known.begin(); i != known.end(); ++i) 
    {
        NodeMap::get_value(i).set_installed(val);
    }
}


void gcomm::evs::Proto::cleanup_joins()
{
    for (NodeMap::iterator i = known.begin(); i != known.end(); ++i)
    {
        NodeMap::get_value(i).set_join_message(0);
    }
}


bool gcomm::evs::Proto::is_all_installed() const
{
    for (NodeMap::const_iterator i = known.begin(); i != known.end(); ++i) 
    {
        const Node& inst(NodeMap::get_value(i));
        if (inst.get_operational() == true && inst.get_installed() == false)
        {
            return false;
        }
    }
    return true;
}


bool gcomm::evs::Proto::is_consensus() const
{
    const JoinMessage* my_jm = 
        NodeMap::get_value(self_i).get_join_message();
    
    if (my_jm == 0) 
    {
        log_debug << self_string() << " no own join message";
        return false;
    }
    
    if (is_consistent_same_view(*my_jm) == false) 
    {
        log_debug << self_string() << " own join message is not consistent";
        return false;
    }
    
    for (NodeMap::const_iterator i = known.begin(); i != known.end(); ++i)
    {
        const Node& inst(NodeMap::get_value(i));
        if (inst.get_operational() == false)
        {
            continue;
        }
        
        const JoinMessage* jm = inst.get_join_message();
        if (jm == 0)
        {
            log_debug << self_string() << " no join message for " 
                      << NodeMap::get_key(i);
            return false;
        }
        
        if (is_consistent(*jm) == false)
        {
            log_debug << self_string() 
                      << " join message not consistent: "
                      << *inst.get_join_message();
            return false;
        }
    }
    log_debug << self_string() << " consensus reached";
    return true;
}

bool gcomm::evs::Proto::is_representative(const UUID& uuid) const
{
    for (NodeMap::const_iterator i = known.begin(); i != known.end(); ++i) 
    {
        if (NodeMap::get_value(i).get_operational()) 
        {
            return (uuid == NodeMap::get_key(i));
        }
    }

    return false;
}


bool gcomm::evs::Proto::is_consistent_input_map(const Message& msg) const
{
    gcomm_assert(msg.get_type() == Message::T_JOIN || 
                 msg.get_type() == Message::T_INSTALL);
    gcomm_assert(msg.get_source_view_id() == current_view.get_id());
    
    Map<const UUID, Range> local_insts, msg_insts;
    
    for (NodeMap::const_iterator i = known.begin(); i != known.end(); ++i)
    {
        const UUID& uuid(NodeMap::get_key(i));
        const Node& inst(NodeMap::get_value(i));
        if (inst.get_operational() == true &&
            inst.get_join_message() != 0 &&
            inst.get_join_message()->get_source_view_id() == current_view.get_id())
        {
            (void)local_insts.insert_checked(make_pair(uuid, im.get_range(uuid)));
        }
    }
    
    assert(msg.has_node_list() == true);
    const MessageNodeList m_insts = msg.get_node_list();
    
    for (MessageNodeList::const_iterator i = m_insts.begin();
         i != m_insts.end(); ++i)
    {
        const UUID& msg_uuid(MessageNodeList::get_key(i));
        const MessageNode& msg_inst(MessageNodeList::get_value(i));
        if (msg_inst.get_operational() == true &&
            msg_inst.get_leaving() == false &&
            msg_inst.get_view_id() == current_view.get_id())
        {
            (void)msg_insts.insert_checked(make_pair(msg_uuid, msg_inst.get_im_range()));
        }
    }
    if (msg_insts != local_insts)
    {
        return false;
    }
    return true;
}

bool gcomm::evs::Proto::is_consistent_partitioning(const Message& msg) const
{
    gcomm_assert(msg.get_type() == Message::T_JOIN ||
                 msg.get_type() == Message::T_INSTALL);
    gcomm_assert(msg.get_source_view_id() == current_view.get_id());
    
    // Compare instances that were present in the current view but are 
    // not proceeding in the next view.
    
    Map<const UUID, Range> local_insts, msg_insts;
    
    for (NodeMap::const_iterator i = known.begin(); i != known.end(); ++i)
    {
        const UUID& uuid(NodeMap::get_key(i));
        const Node& inst(NodeMap::get_value(i));
        if (inst.get_operational() == false &&
            inst.get_leave_message() == 0   &&
            current_view.get_members().find(uuid) != current_view.get_members().end())
        {
            (void)local_insts.insert_checked(make_pair(uuid, 
                                                       im.get_range(uuid)));
        }
    }
    
    assert(msg.has_node_list() == true);
    const MessageNodeList& m_insts = msg.get_node_list();
    
    for (MessageNodeList::const_iterator i = m_insts.begin(); 
         i != m_insts.end(); ++i)
    {
        const UUID& m_uuid(MessageNodeList::get_key(i));
        const MessageNode& m_inst(MessageNodeList::get_value(i));
        if (m_inst.get_operational() == false &&
            m_inst.get_leaving()     == false &&
            m_inst.get_view_id()     == current_view.get_id())
        {
            (void)msg_insts.insert_checked(make_pair(m_uuid, m_inst.get_im_range()));
        }
    }
    if (local_insts != msg_insts)
    {
        return false;
    }
    return true;
}

bool gcomm::evs::Proto::is_consistent_leaving(const Message& msg) const
{
    gcomm_assert(msg.get_type() == Message::T_JOIN ||
                 msg.get_type() == Message::T_INSTALL);
    gcomm_assert(msg.get_source_view_id() == current_view.get_id());
    
    // Compare instances that were present in the current view but are 
    // not proceeding in the next view.
    
    Map<const UUID, Range> local_insts, msg_insts;
    
    for (NodeMap::const_iterator i = known.begin(); i != known.end(); ++i)
    {
        // @todo Using has_leave() is probably wrong, others should
        // delegate leave message if missing on some instance
        const UUID& uuid(NodeMap::get_key(i));
        const Node& inst(NodeMap::get_value(i));
        if (inst.get_operational() == false &&
            has_leave(uuid)        == true  &&
            current_view.get_members().find(uuid) != current_view.get_members().end())
        {
            (void)local_insts.insert_checked(make_pair(uuid, 
                                                       im.get_range(uuid)));
        }
    }
    
    assert(msg.has_node_list() == true);
    const MessageNodeList& m_insts = msg.get_node_list();
    
    for (MessageNodeList::const_iterator i = m_insts.begin(); 
         i != m_insts.end(); ++i)
    {
        const UUID& m_uuid(MessageNodeList::get_key(i));
        const MessageNode& m_inst(MessageNodeList::get_value(i));
        if (m_inst.get_operational() == false &&
            m_inst.get_leaving()     == true &&
            m_inst.get_view_id()     == current_view.get_id())
        {
            (void)msg_insts.insert_checked(make_pair(m_uuid, m_inst.get_im_range()));
        }
    }
    if (local_insts != msg_insts)
    {
        return false;
    }
    return true;
}

bool gcomm::evs::Proto::is_consistent_same_view(const Message& msg) const
{
    gcomm_assert(msg.get_type() == Message::T_JOIN ||
                 msg.get_type() == Message::T_INSTALL);
    gcomm_assert(msg.get_source_view_id() == current_view.get_id());
    
    // Compare aru seqs
    if (im.get_aru_seq() != msg.get_aru_seq())
    {
        log_debug << "aru seq not consistent, local " << im.get_aru_seq()
                  << " msg " << msg.get_aru_seq();
        return false;
    }
    
    // Compare safe seqs
    if (im.get_safe_seq() != msg.get_seq())
    {
        log_debug << "safe seq not consistent, local " << im.get_safe_seq()
                  << " msg " << msg.get_seq();
        return false;
    }
    
    if (is_consistent_input_map(msg) == false)
    {
        log_debug << "input map not consistent";
        return false;
    }
    
    if (is_consistent_partitioning(msg) == false)
    {
        log_debug << "partitioning not consistent";
        return false;
    }
    
    if (is_consistent_leaving(msg) == false)
    {
        log_debug << "leaving not consistent";
        return false;
    }

    return true;
}

bool gcomm::evs::Proto::is_consistent_joining(const Message& msg) const
{
    gcomm_assert(msg.get_type() == Message::T_JOIN ||
                 msg.get_type() == Message::T_INSTALL);
    gcomm_assert(current_view.get_id() != msg.get_source_view_id());
    
    Map<const UUID, Range> local_insts, msg_insts;
    for (NodeMap::const_iterator i = known.begin(); i != known.end(); ++i)
    {
        const UUID& uuid(NodeMap::get_key(i));
        const Node& inst(NodeMap::get_value(i));
        if (inst.get_operational() == false)
        {
            continue;
        }

        const JoinMessage* jm = inst.get_join_message();        
        if (jm == 0)
        {
            return false;
        }
        
        if (msg.get_source_view_id() == jm->get_source_view_id())
        { 
            if (msg.get_aru_seq() != jm->get_aru_seq())
            {
                return false;
            }
            if (msg.get_seq() != jm->get_seq())
            {
                return false;
            }
        }
        local_insts.insert_checked(make_pair(uuid, Range()));
    }
    
    assert(msg.has_node_list() == true);
    const MessageNodeList m_insts = msg.get_node_list();
    
    for (MessageNodeList::const_iterator mi = m_insts.begin();
         mi != m_insts.end(); ++mi)
    {
        const UUID& m_uuid(MessageNodeList::get_key(mi));
        const MessageNode& m_inst(MessageNodeList::get_value(mi));
        
        if (m_inst.get_operational() == true)
        {
            msg_insts.insert_checked(make_pair(m_uuid, Range()));
        }
    }
    
    if (local_insts != msg_insts)
    {
        return false;
    }
    
    return true;
}


bool gcomm::evs::Proto::is_consistent(const Message& msg) const
{
    if (msg.get_source_view_id() == current_view.get_id())
    {
        return is_consistent_same_view(msg);
    }
    else
    {
        return is_consistent_joining(msg);
    }
}


/////////////////////////////////////////////////////////////////////////////
// Message sending
/////////////////////////////////////////////////////////////////////////////

template <class M>
void push_header(const M& msg, WriteBuf* wb)
{
    vector<byte_t> buf(msg.serial_size());
    msg.serialize(&buf[0], buf.size(), 0);
    wb->prepend_hdr(&buf[0], buf.size());
}

template <class M>
void pop_header(const M& msg, WriteBuf* wb)
{
    wb->rollback_hdr(msg.serial_size());
}


bool gcomm::evs::Proto::is_flow_control(const Seqno seq, const Seqno win) const
{
    assert(seq != Seqno::max() && win != Seqno::max());
    
    const Seqno base(im.get_aru_seq() == Seqno::max() ? 0 : im.get_aru_seq());
    if (seq >= base + win)
    {
        return true;
    }
    return false;
}

int gcomm::evs::Proto::send_user(WriteBuf* wb, 
                                 const uint8_t user_type,
                                 const SafetyPrefix sp, 
                                 const Seqno win,
                                 const Seqno up_to_seqno,
                                 bool local)
{
    assert(get_state() == S_LEAVING || 
           get_state() == S_RECOVERY || 
           get_state() == S_OPERATIONAL);
    assert(up_to_seqno >= last_sent);

    int ret;
    Seqno seq = (last_sent == Seqno::max() ? 0 : last_sent + 1);
    
    // Allow flow control only in S_OPERATIONAL state to make 
    // S_RECOVERY state output flush possible.
    if (local                     == false         && 
        get_state()               == S_OPERATIONAL && 
        is_flow_control(seq, win) == true)
    {
        return EAGAIN;
    }
    
    Seqno seq_range = (up_to_seqno == Seqno::max() ? 0 : up_to_seqno - seq);
    assert(seq_range <= 0xff);
    Seqno last_msg_seq = seq + seq_range;
    uint8_t flags;
    
    if (output.size() < 2 || 
        up_to_seqno != Seqno::max() ||
        is_flow_control(last_msg_seq + 1, win) == true)
    {
        flags = 0;
    }
    else 
    {
        flags = Message::F_MSG_MORE;
    }
    
    UserMessage msg(get_uuid(),
                    current_view.get_id(), 
                    seq,
                    im.get_aru_seq(),
                    seq_range,
                    sp, 
                    user_type,
                    flags);
    push_header(msg, wb);
    
    // Insert first to input map to determine correct aru seq
    ReadBuf* rb = wb->to_readbuf();
    const Range range(im.insert(get_uuid(), msg, rb, 0));
    rb->release();
    
    last_sent = last_msg_seq;
    assert(range.get_hs() == last_sent);
    im.set_safe_seq(get_uuid(), im.get_aru_seq());
    pop_header(msg, wb);
    
    if (local == false)
    {
        // Rewrite message hdr to include correct aru
        msg.set_aru_seq(im.get_aru_seq());
        push_header(msg, wb);
        if ((ret = pass_down(wb, 0)))
        {
            log_debug << "pass down: "  << ret;
        }
        pop_header(msg, wb);
    }
    
    if (delivering == false)
    {
        deliver();
    }
    return 0;
}

int gcomm::evs::Proto::send_user()
{
    
    if (output.empty())
        return 0;
    assert(get_state() == S_OPERATIONAL || get_state() == S_RECOVERY);
    pair<WriteBuf*, ProtoDownMeta> wb = output.front();
    int ret;
    if ((ret = send_user(wb.first, wb.second.get_user_type(), 
                         wb.second.get_safety_prefix(), 
                         send_window, Seqno::max())) == 0) {
        output.pop_front();
        delete wb.first;
    }
    return ret;
}


void gcomm::evs::Proto::complete_user(const Seqno high_seq)
{
    log_debug << self_string() << " completing seqno to " << high_seq;;
    WriteBuf wb(0, 0);
    int err = send_user(&wb, 0xff, SP_DROP, send_window, high_seq);
    if (err != 0)
    {
        log_warn << "failed to send completing msg " << err;
    }
}


int gcomm::evs::Proto::send_delegate(WriteBuf* wb)
{
    DelegateMessage dm(get_uuid(), current_view.get_id());
    push_header(dm, wb);
    int ret = pass_down(wb, 0);
    pop_header(dm, wb);
    return ret;
}


void gcomm::evs::Proto::send_gap(const UUID&   range_uuid, 
                                 const ViewId& source_view_id, 
                                 const Range   range)
{
    log_debug << self_string() << " to "  << range_uuid 
              << " requesting range " << range;
    // TODO: Investigate if gap sending can be somehow limited, 
    // message loss happen most probably during congestion and 
    // flooding network with gap messages won't probably make 
    // conditions better
    
    GapMessage gm(get_uuid(),
                  source_view_id,
                  last_sent, 
                  im.get_aru_seq(), 
                  range_uuid, 
                  range);
    
    WriteBuf wb(0, 0);
    push_header(gm, &wb);
    int err = pass_down(&wb, 0);
    if (err != 0)
    {
        log_warn << "send failed " << strerror(err);
    }
    handle_gap(gm, self_i);
}

void gcomm::evs::Proto::populate_node_list(MessageNodeList* node_list) const
{
    for (NodeMap::const_iterator i = known.begin(); i != known.end(); ++i)
    {
        const UUID& uuid = NodeMap::get_key(i);
        const Node& node = NodeMap::get_value(i);
        const bool in_current = (current_view.get_members().find(uuid) != 
                                 current_view.get_members().end());
        const ViewId vid = (node.get_join_message() != 0 ?
                            node.get_join_message()->get_source_view_id() :
                            (in_current == true ? 
                             current_view.get_id() : 
                             ViewId()));
        const Seqno safe_seq = (in_current == true ? im.get_safe_seq(uuid) : Seqno::max());
        const Range range = (in_current == true ? im.get_range(uuid) : Range());
        const MessageNode mnode(node.get_operational(),
                                has_leave(uuid),
                                vid, 
                                safe_seq, 
                                range);
        node_list->insert_checked(make_pair(uuid, mnode));
    }
}

JoinMessage gcomm::evs::Proto::create_join()
{

    MessageNodeList node_list;

    populate_node_list(&node_list);
    JoinMessage jm(get_uuid(),
                   current_view.get_id(),
                   im.get_safe_seq(),
                   im.get_aru_seq(),
                   ++fifo_seq,
                   &node_list);
    
    if (is_consistent(jm) == false)
    {
        gcomm_throw_fatal << "Inconsistent JOIN message";
    }
    return jm;
}


void gcomm::evs::Proto::set_join(const JoinMessage& jm, const UUID& source)
{
    NodeMap::iterator i;
    gu_trace(i = known.find_checked(source));
    NodeMap::get_value(i).set_join_message(new JoinMessage(jm));
}


void gcomm::evs::Proto::set_leave(const LeaveMessage& lm, const UUID& source)
{
    NodeMap::iterator i;
    gu_trace(i = known.find_checked(source));
    Node& inst(NodeMap::get_value(i));
    
    if (inst.get_leave_message())
    {
        log_warn << "Duplicate leave:\told: "
                 << *inst.get_leave_message() 
                 << "\tnew: " << lm;
    }
    else
    {
        inst.set_leave_message(new LeaveMessage(lm));
    }
}


bool gcomm::evs::Proto::has_leave(const UUID& uuid) const
{
    NodeMap::const_iterator ii;
    gu_trace(ii = known.find_checked(uuid));
    const Node& inst(NodeMap::get_value(ii));

    if (inst.get_leave_message() != 0)
    {
        return true;
    }
    
    for (ii = known.begin(); ii != known.end(); ++ii)
    {
        const Node& ii_node(NodeMap::get_value(ii));
        const JoinMessage* jm = ii_node.get_join_message();
        if (ii_node.get_join_message() != 0)
        {
            assert(jm->has_node_list() == true);
            const MessageNodeList& im = jm->get_node_list();
            
            MessageNodeList::const_iterator ji = im.find(uuid);
            
            if (ji != im.end() &&
                MessageNodeList::get_value(ji).get_leaving() == true)
            {
                return true;
            }
        }
    }
    
    return false;
}

void gcomm::evs::Proto::send_join(bool handle)
{
    assert(output.empty() == true);

    JoinMessage jm(create_join());

    log_debug << self_string() << " sending join " << jm;
    
    vector<byte_t> buf(jm.serial_size());
    gu_trace((void)jm.serialize(&buf[0], buf.size(), 0));
    WriteBuf wb(&buf[0], buf.size());
    
    int err = pass_down(&wb, 0);
    if (err != 0) 
    {
        log_warn << "send failed: " << strerror(err);
    }
    
    if (handle)
    {
        handle_join(jm, self_i);
    }
    else
    {
        set_join(jm, get_uuid());
    }
}

void gcomm::evs::Proto::send_leave()
{
    assert(get_state() == S_LEAVING);
    
    log_debug << self_string() << " send leave as " << last_sent;
    
    LeaveMessage lm(get_uuid(),
                    current_view.get_id(),
                    last_sent,
                    im.get_aru_seq(), 
                    ++fifo_seq);

    WriteBuf wb(0, 0);
    push_header(lm, &wb);
    
    int err = pass_down(&wb, 0);
    if (err != 0)
    {
        log_warn << "send failed " << strerror(err);
    }
    
    handle_leave(lm, self_i);
}


struct ViewIdCmp
{
    bool operator()(const NodeMap::value_type& a,
                    const NodeMap::value_type& b) const
    {
        if (b.second.get_join_message() == 0)
        {
            return true;
        }
        else if (a.second.get_join_message() == 0)
        {
            return false;
        }
        else
        {
            return (a.second.get_join_message()->get_source_view_id() <
                    b.second.get_join_message()->get_source_view_id());
        }
    }
};


void gcomm::evs::Proto::send_install()
{
    log_debug << self_string() << " installing flag " << installing;
    
    if (installing)
    {
        log_warn << "install flag is set";
        return;
    }
    
    gcomm_assert(is_consensus() == true && 
                 is_representative(get_uuid()) == true);
    
    NodeMap::const_iterator max_node = 
        max_element(known.begin(), known.end(), ViewIdCmp());
    gcomm_assert(max_node != known.end());

    const uint32_t max_view_id_seq = 
        NodeMap::get_value(max_node).get_join_message()->get_source_view_id().get_seq();
    
    MessageNodeList node_list;
    populate_node_list(&node_list);
    
    InstallMessage imsg(get_uuid(),
                        ViewId(get_uuid(), max_view_id_seq + 1),
                        im.get_safe_seq(),
                        im.get_aru_seq(),
                        ++fifo_seq,
                        &node_list);

    log_info << self_string() << " sending install: " << imsg;
    
    vector<byte_t> buf(imsg.serial_size());
    gu_trace((void)imsg.serialize(&buf[0], buf.size(), 0));
    WriteBuf wb (&buf[0], buf.size());

    int err = pass_down(&wb, 0);;
    if (err != 0) 
    {
        log_warn << "send failed " << strerror(err);
    }
    
    installing = true;
    handle_install(imsg, self_i);
}


void gcomm::evs::Proto::resend(const UUID& gap_source, const Range range)
{
    assert(gap_source != get_uuid());
    assert(range.get_lu() != Seqno::max() && range.get_hs() != Seqno::max());
    assert(range.get_lu() <= range.get_hs());

    if (range.get_lu() <= im.get_safe_seq())
    {
        log_warn << "lu <= safe_seq";
        return;
    }
    
    log_debug << self_string() << " resending, requested by " 
              << gap_source 
              << " " 
              << range.get_lu() << " -> " 
              << range.get_hs();
    
    Seqno seq = range.get_lu(); 
    while (seq <= range.get_hs())
    {
        InputMap::iterator msg_i = im.find(get_uuid(), seq);
        if (msg_i == im.end())
        {
            msg_i = im.recover(get_uuid(), seq);
        }

        const UserMessage& msg(InputMap::MsgIndex::get_value(msg_i).get_msg());
        assert(msg.get_source() == get_uuid());
        const ReadBuf* rb(InputMap::MsgIndex::get_value(msg_i).get_rb());
        UserMessage um(msg.get_source(),
                       msg.get_source_view_id(),
                       msg.get_seq(),
                       im.get_aru_seq(),
                       msg.get_seq_range(),
                       msg.get_safety_prefix(),
                       msg.get_user_type(),
                       Message::F_RETRANS);
        
        WriteBuf wb(rb != 0 ? rb->get_buf() : 0, rb != 0 ? rb->get_len() : 0);
        push_header(um, &wb);
        
        int err = pass_down(&wb, 0);
        if (err != 0)
        {
            log_warn << "retrans failed " << strerror(err);
            break;
        }

        seq = seq + msg.get_seq_range() + 1;
    }
}

void gcomm::evs::Proto::recover(const UUID& gap_source, 
                                const UUID& range_uuid,
                                const Range range)
{
    assert(gap_source != get_uuid());
    assert(range.get_lu() != Seqno::max() && range.get_hs() != Seqno::max());
    assert(range.get_lu() <= range.get_hs());
    
    if (range.get_lu() <= im.get_safe_seq())
    {
        log_warn << "lu <= safe_seq";
        return;
    }
    
    log_debug << self_string() << " recovering message, requested by " 
              << gap_source 
              << " " 
              << range.get_lu() << " -> " 
              << range.get_hs();
    
    Seqno seq = range.get_lu(); 
    while (seq <= range.get_hs())
    {
        InputMap::iterator msg_i = im.find(range_uuid, seq);
        if (msg_i == im.end())
        {
            msg_i = im.recover(range_uuid, seq);
        }
        
        const UserMessage& msg(InputMap::MsgIndex::get_value(msg_i).get_msg());
        assert(msg.get_source() == range_uuid);
        const ReadBuf* rb(InputMap::MsgIndex::get_value(msg_i).get_rb());
        UserMessage um(msg.get_source(),
                       msg.get_source_view_id(),
                       msg.get_seq(),
                       msg.get_aru_seq(),
                       msg.get_seq_range(),
                       msg.get_safety_prefix(),
                       msg.get_user_type(),
                       Message::F_SOURCE | Message::F_RETRANS);
        
        WriteBuf wb(rb != 0 ? rb->get_buf() : 0, rb != 0 ? rb->get_len() : 0);
        push_header(um, &wb);
        
        int err = send_delegate(&wb);
        if (err != 0)
        {
            log_warn << "recovery failed " << strerror(err);
            break;
        }
        seq = seq + msg.get_seq_range() + 1;
    }
}

void gcomm::evs::Proto::handle_foreign(const Message& msg)
{
    if (msg.get_type() == Message::T_LEAVE)
    {
        // No need to handle foreign LEAVE message
        return;
    }
    
    const UUID& source = msg.get_source();
    
    log_debug << self_string() << " detected new source: "
              << source;
    
    NodeMap::iterator i = known.insert_checked(make_pair(source, Node()));
    assert(NodeMap::get_value(i).get_operational() == true);
    
    if (get_state() == S_JOINING || get_state() == S_RECOVERY || 
        get_state() == S_OPERATIONAL)
    {
        log_debug << self_string()
                  << " shift to S_RECOVERY due to foreign message";
        shift_to(S_RECOVERY, true);
    }
    
    // Set join message after shift to recovery, shift may clean up
    // join messages
    if (msg.get_type() == Message::T_JOIN)
    {
        set_join(static_cast<const JoinMessage&>(msg), msg.get_source());
    }
}

void gcomm::evs::Proto::handle_msg(const Message& msg, 
                                   const ReadBuf* rb,
                                   const size_t roff)
{
    if (get_state() == S_CLOSED)
    {
        log_debug << "dropping message in closed state";
        return;
    }
    gcomm_assert(msg.get_source() != UUID::nil() && 
                 msg.get_source() != get_uuid());
    
    
    // Figure out if the message is from known source
    NodeMap::iterator ii = known.find(msg.get_source());
    
    if (ii == known.end())
    {
        handle_foreign(msg);
        return;
    }
    
    // Filter out unwanted/duplicate membership messages
    if (msg.is_membership())
    {
        if (NodeMap::get_value(ii).get_fifo_seq() >= msg.get_fifo_seq())
        {
            log_warn << "dropping non-fifo membership message";
            return;
        }
        else
        {
            NodeMap::get_value(ii).set_fifo_seq(msg.get_fifo_seq());
        }
    }
    else
    {
        // Accept non-membership messages only from current view
        // or from view to be installed
        if (msg.get_source_view_id() != current_view.get_id())
        {
            if (install_message == 0 ||
                install_message->get_source_view_id() != msg.get_source_view_id())
            {
                return;
            }
        }
    }
    
    switch (msg.get_type()) {
    case Message::T_USER:
        handle_user(static_cast<const UserMessage&>(msg), ii, rb, roff);
        break;
    case Message::T_DELEGATE:
        handle_delegate(static_cast<const DelegateMessage&>(msg), ii, rb, roff);
        break;
    case Message::T_GAP:
        handle_gap(static_cast<const GapMessage&>(msg), ii);
        break;
    case Message::T_JOIN:
        handle_join(static_cast<const JoinMessage&>(msg), ii);
        break;
    case Message::T_LEAVE:
        handle_leave(static_cast<const LeaveMessage&>(msg), ii);
        break;
    case Message::T_INSTALL:
        handle_install(static_cast<const InstallMessage&>(msg), ii);
        break;
    default:
        log_warn << "Invalid message type: " << msg.get_type();
    }
}

////////////////////////////////////////////////////////////////////////
// Protolay interface
////////////////////////////////////////////////////////////////////////

size_t gcomm::evs::Proto::unserialize_message(const UUID& source, 
                                              const ReadBuf* const rb, 
                                              size_t offset,
                                              Message* msg)
{


    gu_trace(offset = msg->unserialize(rb->get_buf(), rb->get_len(), offset));
    if ((msg->get_flags() & Message::F_SOURCE) == false)
    {
        gcomm_assert(source != UUID::nil());
        msg->set_source(source);
    }
    
    switch (msg->get_type())
    {
    case Message::T_NONE:
        gcomm_throw_fatal;
        break;
    case Message::T_USER:
        gu_trace(offset = static_cast<UserMessage&>(*msg).unserialize(
                     rb->get_buf(), rb->get_len(), offset, true));
        break;
    case Message::T_DELEGATE:
        gu_trace(offset = static_cast<DelegateMessage&>(*msg).unserialize(
                     rb->get_buf(), rb->get_len(), offset, true));
        break;
    case Message::T_GAP:
        gu_trace(offset = static_cast<GapMessage&>(*msg).unserialize(
                     rb->get_buf(), rb->get_len(), offset, true));
        break;
    case Message::T_JOIN:
        gu_trace(offset = static_cast<JoinMessage&>(*msg).unserialize(
                     rb->get_buf(), rb->get_len(), offset, true));
        break;
    case Message::T_INSTALL:
        gu_trace(offset = static_cast<InstallMessage&>(*msg).unserialize(
                     rb->get_buf(), rb->get_len(), offset, true));
        break;
    case Message::T_LEAVE:
        gu_trace(offset = static_cast<LeaveMessage&>(*msg).unserialize(
                     rb->get_buf(), rb->get_len(), offset, true));
        break;
    }
    return offset;
}

void gcomm::evs::Proto::handle_up(int cid, 
                                  const ReadBuf* rb, 
                                  size_t offset,
                                  const ProtoUpMeta* um)
{
    Critical crit(mon);
    
    Message msg;
    
    if (rb == 0 || um == 0)
        gcomm_throw_fatal << "Invalid input: rb == 0 || um == 0";
    
    if (get_state() == S_CLOSED)
    {
        log_debug << "dropping message in closed state";
        return;
    }
    
    gcomm_assert(um->get_source() != UUID::nil());    
    if (um->get_source() == get_uuid())
    {
        log_warn << "dropping self originated message";
    }
    
    gu_trace(offset = unserialize_message(um->get_source(), rb, offset, &msg));
    handle_msg(msg, rb, offset);
}

int gcomm::evs::Proto::handle_down(WriteBuf* wb, const ProtoDownMeta* dm)
{
    Critical crit(mon);
    
    if (get_state() == S_RECOVERY)
    {
        return EAGAIN;
    }

    else if (get_state() != S_OPERATIONAL)
    {
        log_warn << "user message in state " << to_string(get_state());
        return ENOTCONN;
    }

    if (dm && dm->get_user_type() == 0xff)
    {
        return EINVAL;
    }
    
    int ret = 0;
    
    if (output.empty()) 
    {
        int err = send_user(wb, 
                            static_cast<uint8_t>(dm ? dm->get_user_type() : 0xff),
                            dm->get_safety_prefix(), send_window.get()/2, 
                            Seqno::max());
        switch (err) 
        {
        case EAGAIN:
        {
            WriteBuf* priv_wb = wb->copy();
            output.push_back(make_pair(priv_wb, dm ? ProtoDownMeta(*dm) : ProtoDownMeta(0xff)));
            // Fall through
        }
        case 0:
            break;
        default:
            log_error << "Send error: " << err;
            ret = err;
        }
    } 
    else if (output.size() < max_output_size)
    {
        WriteBuf* priv_wb = wb->copy();
        output.push_back(make_pair(priv_wb, dm ? ProtoDownMeta(*dm) : ProtoDownMeta(0xff)));
    } 
    else 
    {
        ret = EAGAIN;
    }
    return ret;
}

/////////////////////////////////////////////////////////////////////////////
// State handler
/////////////////////////////////////////////////////////////////////////////

void gcomm::evs::Proto::shift_to(const State s, const bool send_j)
{
    if (shift_to_rfcnt > 0) gcomm_throw_fatal;

    shift_to_rfcnt++;

    static const bool allowed[S_MAX][S_MAX] = {
        // CLOSED JOINING LEAVING RECOV  OPERAT
        {  false,  true,   false, false, false }, // CLOSED
        
        {  false,  false,  true,  true,  false }, // JOINING

        {  true,   false,  false, false, false }, // LEAVING

        {  false,  false,  true,  true,  true  }, // RECOVERY

        {  false,  false,  true,  true,  false }  // OPERATIONAL
    };
    
    assert(s < S_MAX);
    
    if (allowed[state][s] == false) {
        gcomm_throw_fatal << "Forbidden state transition: " 
                          << to_string(state) << " -> " << to_string(s);
    }
    
    if (get_state() != s)
    {
        log_debug << self_string() << ": state change: " 
                  << to_string(state) << " -> " << to_string(s);
    }
    switch (s) {
    case S_CLOSED:
        if (collect_stats)
        {
            log_debug << "delivery stats (safe): " << hs_safe.to_string();
        }
        hs_safe.clear();
        stop_inactivity_timer();
        cleanup_unoperational();
        cleanup_views();
        cleanup();
        state = S_CLOSED;
        break;
    case S_JOINING:
        // tp->set_loopback(true);
        state = S_JOINING;
        start_inactivity_timer();
        break;
    case S_LEAVING:
        // send_leave();
        // tp->set_loopback(true);
        unset_consensus_timer();
        state = S_LEAVING;
        break;
    case S_RECOVERY:
    {
        // tp->set_loopback(true);
        stop_resend_timer();
        stop_send_join_timer();
        start_send_join_timer();
        if (get_state() != S_RECOVERY)
        {
            cleanup_joins();
        }
        setall_installed(false);
        delete install_message;
        install_message = 0;
        installing = false;
        if (is_set_consensus_timer())
        {
            unset_consensus_timer();
        }
        set_consensus_timer();
        state = S_RECOVERY;
        log_debug << self_string() << " shift to recovery, flushing "
                  << output.size() << " messages";
        while (output.empty() == false)
        {
            send_user();
        }
        if (send_j == true)
        {
            send_join(false);
        }
        
        break;
    }
    case S_OPERATIONAL:
    {
        // tp->set_loopback(false);
        assert(output.empty() == true);
        assert(install_message && (is_representative(get_uuid()) == false 
                                   || is_consistent(*install_message)));
        assert(is_all_installed() == true);
        unset_consensus_timer();
        stop_send_join_timer();
        deliver();
        deliver_trans_view(false);
        deliver_trans();
        // Reset input map
        im.clear();
        
        previous_view = current_view;
        previous_views.push_back(make_pair(current_view.get_id(), Time::now()));
        
        gcomm_assert(install_message->has_node_list() == true);
        const MessageNodeList& imap = install_message->get_node_list();
        
        for (MessageNodeList::const_iterator i = imap.begin();
             i != imap.end(); ++i)
        {
            previous_views.push_back(make_pair(MessageNodeList::get_value(i).get_view_id(), 
                                               Time::now()));
        }
        current_view = View(View::V_REG, 
                            install_message->get_source_view_id());
        for (NodeMap::const_iterator i = known.begin(); i != known.end(); ++i)
        {
            if (NodeMap::get_value(i).get_installed() == true)
            {
                current_view.add_member(NodeMap::get_key(i), "");
                im.insert_uuid(NodeMap::get_key(i));
            }
        }
        
        last_sent = Seqno::max();
        state = S_OPERATIONAL;
        deliver_reg_view();
        if (collect_stats)
        {
            log_debug << "delivery stats (safe): " + hs_safe.to_string();
        }
        hs_safe.clear();
        cleanup_unoperational();
        cleanup_views();
        for (NodeMap::iterator i = known.begin(); i != known.end(); ++i)
        {
            NodeMap::get_value(i).set_join_message(0);
        }
        delete install_message;
        install_message = 0;
        log_debug << "new view: " << current_view;
        start_resend_timer();
        assert(get_state() == S_OPERATIONAL);
        break;
    }
    default:
        gcomm_throw_fatal << "Invalid state";
    }
    shift_to_rfcnt--;
}

////////////////////////////////////////////////////////////////////////////
// Message delivery
////////////////////////////////////////////////////////////////////////////

void gcomm::evs::Proto::validate_reg_msg(const UserMessage& msg)
{
    if (msg.get_source_view_id() != current_view.get_id())
    {
        gcomm_throw_fatal << "Reg validate: not current view";
    }
    if (collect_stats == true && msg.get_safety_prefix() == SP_SAFE)
    {
        Time now(Time::now());
        hs_safe.insert(now.to_double() - msg.get_tstamp().to_double());
    }
}

void gcomm::evs::Proto::deliver()
{
    if (delivering == true)
    {
        gcomm_throw_fatal << "Recursive enter to delivery";
    }
    
    delivering = true;

    if (get_state() != S_OPERATIONAL && get_state() != S_RECOVERY && 
        get_state() != S_LEAVING)
        gcomm_throw_fatal << "Invalid state";

    log_debug << "aru_seq: "   << im.get_aru_seq() 
              << " safe_seq: " << im.get_safe_seq();
    
    InputMap::MsgIndex::iterator i, i_next;
    for (i = im.begin(); i != im.end(); i = i_next)
    {
        i_next = i;
        ++i_next;
        const InputMap::Msg& msg(InputMap::MsgIndex::get_value(i));
        validate_reg_msg(msg.get_msg());
        bool deliver = false;
        switch (msg.get_msg().get_safety_prefix())
        {
        case SP_DROP:
            deliver = true;
            break;
            
        case SP_SAFE:
            if (im.is_safe(i) == true)
            {
                deliver = true;
            }
            break;
        case SP_AGREED:
            if (im.is_agreed(i) == true)
            {
                deliver = true;
            }
            break;
        case SP_FIFO:
            if (im.is_fifo(i) == true)
            {
                deliver = true;
            }
            break;
        default:
            gcomm_throw_fatal;
        }
        
        if (deliver == true)
        {
            if (msg.get_msg().get_safety_prefix() != SP_DROP)
            {
                ProtoUpMeta um(msg.get_uuid(), 
                               msg.get_msg().get_user_type());
                pass_up(msg.get_rb(), 0, &um);
            }
            im.erase(i);
        }
    }
    delivering = false;

}

void gcomm::evs::Proto::validate_trans_msg(const UserMessage& msg)
{
    log_debug << msg;

    if (msg.get_source_view_id() != current_view.get_id())
    {
        // @todo: do we have to freak out here?
        gcomm_throw_fatal << "Reg validate: not current view";
    }
    
    if (collect_stats && msg.get_safety_prefix() == SP_SAFE)
    {
        Time now(Time::now());
        hs_safe.insert(now.to_double() - msg.get_tstamp().to_double());
    }
}

void gcomm::evs::Proto::deliver_trans()
{
    if (delivering == true)
    {
        gcomm_throw_fatal << "Recursive enter to delivery";
    }
    
    delivering = true;
    
    if (get_state() != S_RECOVERY && get_state() != S_LEAVING)
        gcomm_throw_fatal << "Invalid state";
    
    // In transitional configuration we must deliver all messages that 
    // are fifo. This is because:
    // - We know that it is possible to deliver all fifo messages originated
    //   from partitioned component as safe in partitioned component
    // - Aru in this component is at least the max known fifo seq
    //   from partitioned component due to message recovery 
    // - All FIFO messages originated from this component must be 
    //   delivered to fulfill self delivery requirement and
    // - FIFO messages originated from this component qualify as AGREED
    //   in transitional configuration

    InputMap::iterator i, i_next;
    for (i = im.begin(); i != im.end(); i = i_next)
    {
        i_next = i;
        ++i_next;    
        const InputMap::Msg& msg(InputMap::MsgIndex::get_value(i));
        validate_reg_msg(msg.get_msg());
        bool deliver = false;
        switch (msg.get_msg().get_safety_prefix())
        {
        case SP_DROP:
            deliver = true;
            break;
        case SP_SAFE:
        case SP_AGREED:
        case SP_FIFO:
            if (im.is_fifo(i) == true)
            {
                deliver = true;
            }
            break;
        default:
            gcomm_throw_fatal;
        }
        
        if (deliver == true)
        {
            if (msg.get_msg().get_safety_prefix() != SP_DROP)
            {
                ProtoUpMeta um(msg.get_uuid(), 
                               msg.get_msg().get_user_type());
                pass_up(msg.get_rb(), 0, &um);
            }
            im.erase(i);
        }
    }
    
    // Sanity check:
    // There must not be any messages left that 
    // - Are originated from outside of trans conf and are FIFO
    // - Are originated from trans conf
    for (i = im.begin(); i != im.end(); i = i_next)
    {
        i_next = i;
        ++i_next;
        const InputMap::Msg& msg(InputMap::MsgIndex::get_value(i));
        NodeMap::iterator ii;
        gu_trace(ii = known.find_checked(msg.get_uuid()));

        if (NodeMap::get_value(ii).get_installed() == true)
        {
            gcomm_throw_fatal << "Protocol error in transitional delivery "
                              << "(self delivery constraint)";
        }
        else if (im.is_fifo(i) == true)
        {
            gcomm_throw_fatal << "Protocol error in transitional delivery "
                              << "(fifo from partitioned component)";
        }
        im.erase(i);
    }
    delivering = false;
}


/////////////////////////////////////////////////////////////////////////////
// Message handlers
/////////////////////////////////////////////////////////////////////////////


void gcomm::evs::Proto::handle_user(const UserMessage& msg, 
                                    NodeMap::iterator ii, 
                                    const ReadBuf* rb, 
                                    const size_t roff)
{
    assert(ii != known.end());
    Node& inst(NodeMap::get_value(ii));
    
    if (msg.get_flags() & Message::F_RETRANS)
    {
        log_debug << self_string() << " msg with retrans flag " << msg;
    }
    else 
    {
        
        log_debug << self_string() << " " << msg;
    }
    
    if (state == S_JOINING || state == S_CLOSED) 
    {
        // Drop message
        log_debug << self_string() << " dropping: " << msg;
        return;
    } 
    else if (msg.get_source_view_id() != current_view.get_id()) 
    {
        if (get_state() == S_LEAVING) 
        {
            log_debug << self_string() << " leaving, dropping: " 
                      << msg;
            return;
        }
        
        if (msg_from_previous_view(previous_views, msg))
        {
            log_debug << self_string() << " user message " 
                      << msg 
                      << " from previous view";
            return;
        }
        
        if (inst.get_operational() == false) 
        {
            // This is probably partition merge, see if it works out
            log_debug << self_string() << " unoperational source " 
                      << msg;
            inst.set_operational(true);
            shift_to(S_RECOVERY);
            return;
        } 
        else if (inst.get_installed() == false) 
        {
            if (install_message && 
                msg.get_source_view_id() == install_message->get_source_view_id()) 
            {
                assert(state == S_RECOVERY);
                gcomm_assert(install_message->has_node_list() == true);
                log_debug << self_string() << " recovery user message "
                          << msg;
                
                // Other instances installed view before this one, so it is 
                // safe to shift to S_OPERATIONAL if consensus has been reached
                for (MessageNodeList::const_iterator
                         mi = install_message->get_node_list().begin(); 
                     mi != install_message->get_node_list().end(); ++mi)
                {
                    NodeMap::iterator jj;
                    gu_trace(jj = known.find_checked(
                                 MessageNodeList::get_key(mi)));
                    NodeMap::get_value(jj).set_installed(true);
                }
                
                if (is_consensus() == true) 
                {
                    shift_to(S_OPERATIONAL);
                } 
                else 
                {
                    shift_to(S_RECOVERY);
                    return;
                }
            } 
            else
            {
                return;
            }
        } 
        else 
        {
            log_info << self_string() << " unknown user message: "
                     << msg;
            return;
        }
    }
    
    assert(msg.get_source_view_id() == current_view.get_id());
    
    const Seqno prev_aru(im.get_aru_seq());
    const Seqno prev_safe(im.get_safe_seq());
    const Range prev_range(im.get_range(msg.get_source()));
    const Range range(im.insert(msg.get_source(), msg, rb, roff));
    
    if (range.get_lu() > prev_range.get_lu())
    {
        inst.set_tstamp(Time::now());
    }
    
    // Some messages are missing
    // 
    // TODO: 
    // - Gap messages should take list of gaps to avoid sending gap 
    //   message for each missing message
    // - There should be guard (timer etc) to avoid sending gap message
    //   for each incoming packet from the source of missing packet 
    //   (maybe this is not too bad if gap message contains gap list
    //   and message loss is infrequent)
    if (range.get_hs() > range.get_lu() && 
        (msg.get_flags() & Message::F_RETRANS) == false)
    {
        log_debug << "requesting at " 
                  << self_string() 
                  << " from " 
                  << msg.get_source() 
                  << " " << range 
                  << " due to input map gap, aru " 
                  << im.get_aru_seq();
        send_gap(msg.get_source(), current_view.get_id(), range);
    }
    
    if (output.empty() == true &&
        (msg.get_flags() & Message::F_MSG_MORE) == false &&
        (last_sent == Seqno::max() || last_sent < range.get_hs()))
    {
        // Message not originated from this instance, output queue is empty
        // and last_sent seqno should be advanced
        complete_user(range.get_hs());
    }
    else if ((output.empty() == true && im.get_aru_seq() != prev_aru) ||
             get_state() == S_LEAVING)
    {
        // Output queue empty and aru changed, send gap to inform others
        // @todo Why doing this always in leaving state?
        log_debug << self_string() << " sending empty gap";
        send_gap(UUID(), current_view.get_id(), Range());
    }
    
    deliver();
    while (output.empty() == false)
    {
        if (send_user())
            break;
    }
    
    
    if (get_state() == S_RECOVERY && last_sent == im.get_aru_seq() && 
        (prev_aru != im.get_aru_seq() ||
         prev_safe != im.get_safe_seq()))
    {
        assert(output.empty() == true);
        const JoinMessage* jm = NodeMap::get_value(self_i).get_join_message();
        if (jm == 0 || is_consistent(*jm) == false)
        {
            send_join();
        }
    }
}

void gcomm::evs::Proto::handle_delegate(const DelegateMessage& msg, 
                                        NodeMap::iterator ii,
                                        const ReadBuf* rb, 
                                        size_t offset)
{
    assert(ii != known.end());

    Message umsg;
    gu_trace(offset = unserialize_message(UUID::nil(), rb, offset, &umsg));
    handle_msg(umsg, rb, offset);
}

void gcomm::evs::Proto::handle_gap(const GapMessage& msg, NodeMap::iterator ii)
{
    assert(ii != known.end());
    Node& inst(NodeMap::get_value(ii));
    log_debug << self_string() << " " << msg;
    
    if (get_state() == S_JOINING || get_state() == S_CLOSED) 
    {	
        // Silent drop
        return;
    } 
    else if (get_state() == S_RECOVERY && 
             install_message != 0 && 
             install_message->get_source_view_id() == msg.get_source_view_id()) 
    {
        log_info << self_string() << " install gap " << msg;
        inst.set_installed(true);
        if (is_all_installed() == true)
            shift_to(S_OPERATIONAL);
        return;
    } 
    else if (msg.get_source_view_id() != current_view.get_id()) 
    {
        if (msg_from_previous_view(previous_views, msg) == true)
        {
            log_debug << "gap message from previous view";
            return;
        }
        if (inst.get_operational() == false) 
        {
            // This is probably partition merge, see if it works out
            inst.set_operational(true);
            shift_to(S_RECOVERY);
        } 
        else if (inst.get_installed() == false) 
        {
            // Probably caused by network partitioning during recovery
            // state, this will most probably lead to view 
            // partition/remerge. In order to do it in organized fashion,
            // don't trust the source instance during recovery phase.
            // Note: setting other instance to non-trust here is too harsh
            // LOG_WARN("Setting source status to no-trust");
        } 
        else 
        {
            log_debug << self_string() << " unknown gap message: " << msg;
        }
        return;
    }
    
    assert(msg.get_source_view_id() == current_view.get_id());
    
    const Seqno prev_safe(im.get_safe_seq());
    if (msg.get_aru_seq() != Seqno::max())
    {
        im.set_safe_seq(msg.get_source(), msg.get_aru_seq());
    }

    // Scan through gap list and resend or recover messages if appropriate.
    log_debug << "range uuid " << msg.get_range_uuid();
    if (msg.get_range_uuid() == get_uuid())
    {
        resend(msg.get_source(), msg.get_range());
    }
    else if (get_state() == S_RECOVERY && msg.get_range_uuid() != UUID::nil())
    {
        recover(msg.get_source(), msg.get_range_uuid(), msg.get_range());
    }
    
    // Deliver messages 
    deliver();
    while (get_state() == S_OPERATIONAL && output.empty() == false)
    {
        if (send_user())
            break;
    }
    
    if (get_state() == S_RECOVERY && last_sent == im.get_aru_seq() &&
        prev_safe != im.get_safe_seq())
    {
        assert(output.empty() == true);
        const JoinMessage* jm(NodeMap::get_value(self_i).get_join_message());
        if (jm == 0 || is_consistent(*jm) == false)
        {
            send_join();
        }
    }
}

template <class C>
class SameViewSelect
{
public:
    SameViewSelect(C& c_, const ViewId& view_id_) : c(c_), view_id(view_id_) { }
    
    void operator()(const MessageNodeList::value_type& vt) const
    {
        if (MessageNodeList::get_value(vt).get_view_id() == view_id)
        {
            c.push_back(vt);
        }
    }
private:
    C& c;
    const ViewId& view_id;
};


class RangeHsCmp
{
public:
    bool operator()(const MessageNodeList::value_type& a,
                  const MessageNodeList::value_type& b) const
    {
        if (MessageNodeList::get_value(a).get_im_range().get_hs() == Seqno::max())
        {
            return true;
        }
        else if (MessageNodeList::get_value(b).get_im_range().get_hs() == Seqno::max())
        {
            return false;
        }
        else
        {
            return MessageNodeList::get_value(a).get_im_range().get_hs() < 
                MessageNodeList::get_value(b).get_im_range().get_hs();
        }
    }
};

class RangeLuCmp
{
public:
    bool operator()(const MessageNodeList::value_type& a,
                    const MessageNodeList::value_type& b) const
    {
        if (MessageNodeList::get_value(a).get_im_range().get_lu() == Seqno::max())
        {
            return true;
        }
        else if (MessageNodeList::get_value(b).get_im_range().get_lu() == Seqno::max())
        {
            return false;
        }
        else
        {
            return MessageNodeList::get_value(a).get_im_range().get_lu() < 
                MessageNodeList::get_value(b).get_im_range().get_lu();
        }
    }
};


bool gcomm::evs::Proto::states_compare(const JoinMessage& msg) 
{
    assert(msg.has_node_list() == true);
    const MessageNodeList& node_list(msg.get_node_list());
    bool send_join_p(false);
    
    // Compare view of operational instances
    for (MessageNodeList::const_iterator ii = node_list.begin(); 
         ii != node_list.end(); ++ii) 
    {
        const UUID& msg_node_uuid(MessageNodeList::get_key(ii));
        const MessageNode& msg_node(MessageNodeList::get_value(ii));
        NodeMap::iterator local_ii = known.find(msg_node_uuid);
        if (local_ii == known.end())
        {
            // Don't insert here into known nodes, wait for 
            // direct mutual communication.
            log_debug << self_string() 
                      << ": unknown instance " << msg_node_uuid 
                      << " in join message";
        }
        else 
        {
            Node& local_node(NodeMap::get_value(local_ii));
            if (local_node.get_operational() != msg_node.get_operational()) 
            {
                if (local_node.get_operational() == true && 
                    NodeMap::get_key(local_ii) != get_uuid()) 
                {
                    if (local_node.get_tstamp() + inactive_timeout < Time::now() ||
                        msg_node.get_leaving() == true) 
                    {
                        log_debug << self_string()
                                  << "setting " << NodeMap::get_key(local_ii) 
                                  << " as unoperational";
                        local_node.set_operational(false);
                        send_join_p = true;
                    }
                } 
                else 
                {
                    send_join_p = true;
                }
            }
        }
        
        if (msg_node.get_view_id() == current_view.get_id())
        {
            const Seqno imseq(im.get_safe_seq(msg_node_uuid));
            if ((imseq == Seqno::max() &&
                 msg_node.get_safe_seq() != Seqno::max()) ||
                (imseq != Seqno::max() && 
                 msg_node.get_safe_seq() != Seqno::max() && 
                 imseq < msg_node.get_safe_seq()))
            {
                im.set_safe_seq(msg_node_uuid, msg.get_seq());
                send_join_p = true;
            }
        }
    }

    list<MessageNodeList::value_type> same_view;
    for_each(node_list.begin(), node_list.end(), 
             SameViewSelect<list<MessageNodeList::value_type> >(
                 same_view, current_view.get_id()));
    
    list<MessageNodeList::value_type>::const_iterator high_ii = 
        max_element(same_view.begin(), same_view.end(), RangeHsCmp());
    list<MessageNodeList::value_type>::const_iterator low_ii =
        min_element(same_view.begin(), same_view.end(), RangeLuCmp());

    const Seqno high_seq(high_ii->second.get_im_range().get_hs());
    const Seqno low_seq(low_ii->second.get_im_range().get_lu());
    const UUID& low_uuid(low_ii->first);
    // If this fires, not enough input validation is made
    gcomm_assert(low_seq != Seqno::max());
    
    gcomm_assert(output.empty() == true);
    
    if (high_seq != Seqno::max())
    {
        if (last_sent == Seqno::max() || last_sent < high_seq)
        {
            complete_user(high_seq);
        }
        else if (msg.get_source() != get_uuid() && msg.get_source() == low_uuid)
        {
            resend(msg.get_source(), Range(low_seq, high_seq));
        }
        send_join_p = true;
        
        if (msg.get_source() != get_uuid())
        {
            for (NodeMap::const_iterator i = known.begin(); i != known.end(); 
                 ++i)
            {
                if (NodeMap::get_value(i).get_operational() == false &&
                    current_view.get_members().find(NodeMap::get_key(i)) != 
                    current_view.get_members().end())
                {
                    recover(msg.get_source(), NodeMap::get_key(i), 
                            Range(low_seq, high_seq));
                }
            }
        }
    }
    
    return send_join_p;
}



void gcomm::evs::Proto::handle_join(const JoinMessage& msg, NodeMap::iterator ii)
{
    assert(ii != known.end());
    Node& inst(NodeMap::get_value(ii));
    
    log_debug << self_string() << " view" << current_view.get_id()
              << " ================ enter handle_join ==================";
    log_debug << self_string() << " " << msg;
    
    if (get_state() == S_LEAVING) 
    {
        log_debug << "================ leave handle_join ==================";
        return;
    }
    
    if (msg_from_previous_view(previous_views, msg))
    {
        log_debug << self_string() 
                  << " join message from one of the previous views " 
                  << msg.get_source_view_id();
        log_debug << "================ leave handle_join ==================";
        return;
    }
    
    if (install_message)
    {
        log_debug << self_string() 
                  << " install message and received join, discarding";
        log_debug << "================ leave handle_join ==================";
        return;
    }
    
    
    inst.set_tstamp(Time::now());
    
    bool pre_consistent = is_consistent(msg);
    
    if (get_state() == S_RECOVERY && install_message && pre_consistent) 
    {
        log_debug << self_string() << " redundant join message: "
                  << msg << " install message: "
                  << *install_message;
        log_debug << "================ leave handle_join ==================";
        return;
    }
    
    if ((get_state() == S_OPERATIONAL || install_message) && pre_consistent)
    {
        log_debug << self_string() << " redundant join message in state "
                  << to_string(get_state()) << ": "
                  << msg << " install message: "
                  << *install_message;
        log_debug << "================ leave handle_join ==================";
        return;
    }
    
    bool send_join_p = false;
    if (get_state() == S_JOINING || get_state() == S_OPERATIONAL)
    {
        send_join_p = true;
        shift_to(S_RECOVERY, false);
    }
    
    assert(inst.get_installed() == false);
    
    // Instance previously declared unoperational seems to be operational now
    if (inst.get_operational() == false) 
    {
        inst.set_operational(true);
        log_debug << "unop -> op";
        send_join_p = true;
    } 
    
    // Store join message
    set_join(msg, msg.get_source());
    
    if (msg.get_source_view_id() == current_view.get_id()) 
    {
        const Seqno prev_safe(im.get_safe_seq());
        if (msg.get_aru_seq() != Seqno::max())
        {
            im.set_safe_seq(msg.get_source(), msg.get_aru_seq());
        }

        if (prev_safe != im.get_safe_seq())
        {
            log_debug << "safe seq " 
                      << prev_safe << " -> " << im.get_safe_seq();
        }

        // Aru seqs are not the same
        if (msg.get_aru_seq() != im.get_aru_seq())
        {
            states_compare(msg);
            return;
        }
        
        // Safe seqs are not the same
        if (msg.get_seq() != im.get_safe_seq())
        {
            states_compare(msg);
            return;
        }
    }
    
    
    // Converge towards consensus
    const MessageNodeList& instances = msg.get_node_list();
    MessageNodeList::const_iterator selfi = instances.find(get_uuid());
    if (selfi == instances.end()) 
    {
        // Source instance does not know about this instance, so there 
        // is no sense to compare states yet
        log_debug << "this instance not known by source instance";
        send_join_p = true;
    } 
    else if (current_view.get_id() != msg.get_source_view_id()) 
    {
        // Not coming from same views, there's no point to compare 
        // states further
        log_debug << self_string() 
                  << " join from different view " 
                  << msg.get_source_view_id();
        if (is_consistent(msg) == false)
        {
            send_join_p = true;
        }
    } 
    else 
    {
        log_debug << "states compare";
        if (states_compare(msg) == true)
        {
            send_join_p = true;
        }
    }
    
    const JoinMessage* self_join = NodeMap::get_value(self_i).get_join_message();
    if (((self_join == 0 || is_consistent(*self_join) == false) &&
         send_join_p == true) || pre_consistent == false)
    {
        send_join_p = true;
    }
    else
    {
        send_join_p = false;
    }
    
    set_join(create_join(), get_uuid());
    
    if (is_consensus())
    { 
        if (is_representative(get_uuid()))
        {
            log_info << self_string() << " is consensus and representative";
            send_install();
        }
        else if (pre_consistent == false)
        {
            send_join(false);
        }
    }    
    else if (send_join_p == true && output.empty() == true)
    {
        send_join(false);
    }
    log_debug << "send_join_p: " << send_join_p 
              << " output empty: " << output.empty();
    log_debug << "================ leave handle_join ==================";
}


void gcomm::evs::Proto::handle_leave(const LeaveMessage& msg, 
                                     NodeMap::iterator ii)
{
    assert(ii != known.end());
    Node& inst(NodeMap::get_value(ii));
    log_info << self_string() << "leave message " << msg;
    
    set_leave(msg, msg.get_source());
    if (msg.get_source() == get_uuid()) 
    {
        /* Move all pending messages from output to input map */
        while (output.empty() == false)
        {
            pair<WriteBuf*, ProtoDownMeta> wb = output.front();
            
            if (send_user(wb.first, 
                          wb.second.get_user_type(), 
                          wb.second.get_safety_prefix(), 
                          0, Seqno::max(), true) != 0)
            {
                gcomm_throw_fatal << "send_user() failed";
            }
            
            output.pop_front();
            delete wb.first;
        }
        
        /* Deliver all possible messages in reg view */
        deliver();
        setall_installed(false);
        inst.set_installed(true);
        deliver_trans_view(true);
        deliver_trans();
        deliver_empty_view();
        shift_to(S_CLOSED);
    } 
    else 
    {
        if (msg_from_previous_view(previous_views, msg))
        {
            log_debug << "leave message from previous view";
            return;
        }
        inst.set_operational(false);
        shift_to(S_RECOVERY, true);
        if (is_consensus() && is_representative(get_uuid()))
        {
            send_install();
        }
    }
}

void gcomm::evs::Proto::handle_install(const InstallMessage& msg, 
                                       NodeMap::iterator ii)
{
    
    assert(ii != known.end());
    Node& inst(NodeMap::get_value(ii));
    
    if (get_state() == S_LEAVING) 
    {
        log_debug << "dropping install message in leaving state";
        return;
    }
    
    log_info << self_string() << " " << msg;
    
    if (state == S_JOINING || state == S_CLOSED) 
    {
        log_debug << "dropping install message from " << msg.get_source();
        return;
    } 
    else if (inst.get_operational() == false) 
    {
        log_debug << "setting other as operational";
        inst.set_operational(true);
        shift_to(S_RECOVERY);
        return;
    } 
    else if (msg_from_previous_view(previous_views, msg))
    {
        log_debug << "install message from previous view";
        return;
    }
    else if (install_message)
    {
        if (is_consistent(msg) && 
            msg.get_source_view_id() == install_message->get_source_view_id())
        {
            return;
        }
        log_debug << "";
        shift_to(S_RECOVERY);
        return;
    }
    else if (inst.get_installed() == true) 
    {
        log_debug << "";
        shift_to(S_RECOVERY);
        return;
    } 
    else if (is_representative(msg.get_source()) == false) 
    {
        log_warn << "source is not supposed to be representative";
        shift_to(S_RECOVERY);
        return;
    } 
    
    
    assert(install_message == 0);
    
    if (is_consistent(msg) == true)
    {
        install_message = new InstallMessage(msg);
        send_gap(get_uuid(), install_message->get_source_view_id(), Range());
    }
    else
    {
        log_warn << self_string() 
                 << " install message " 
                 << msg 
                 << " not consistent with state";
        shift_to(S_RECOVERY, true);
    }
}

