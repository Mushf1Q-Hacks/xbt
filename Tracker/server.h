#pragma once

#include "config.h"
#include "tracker_input.h"

class Cstats
{
public:
	long long announced() const
	{
		return announced_http + announced_udp;
	}

	long long scraped() const
	{
		return scraped_http + scraped_udp;
	}

	long long accept_errors = 0;
	long long accepted_tcp = 0;
	long long announced_http = 0;
	long long announced_udp = 0;
	long long received_udp = 0;
	long long rejected_tcp = 0;
	long long scraped_full = 0;
	long long scraped_http = 0;
	long long scraped_multi = 0;
	long long scraped_udp = 0;
	long long sent_udp = 0;
	long long slow_tcp = 0;
	time_t start_time = time(NULL);
};

class peer_key_c
{
public:
	peer_key_c(int host, int uid)
	{
		host_ = host;
#ifdef PEERS_KEY
		uid_ = uid;
#else
		(void)uid;
#endif
	}

	bool operator==(peer_key_c v) const
	{
#ifdef PEERS_KEY
		return host_ == v.host_ && uid_ == v.uid_;
#else
		return host_ == v.host_;
#endif
	}

	bool operator<(peer_key_c v) const
	{
#ifdef PEERS_KEY
		return host_ < v.host_ || host_ == v.host_ && uid_ < v.uid_;
#else
		return host_ < v.host_;
#endif
	}

	friend std::size_t hash_value(const peer_key_c& v)
	{
		std::size_t seed = boost::hash_value(v.host_);
#ifdef PEERS_KEY
		boost::hash_combine(seed, v.uid_);
#endif
		return seed;
	}

	int host_;
#ifdef PEERS_KEY
	int uid_;
#endif
};

struct t_peer
{
	long long downloaded;
	long long uploaded;
	time_t mtime = 0;
	int uid;
	short port;
	bool left;
	std::array<char, 20> peer_id;
};

struct t_torrent
{
	void select_peers(mutable_str_ref& d, const Ctracker_input&) const;

	boost::unordered_map<peer_key_c, t_peer> peers;
	time_t ctime;
	int completed = 0;
	int fid = 0;
	int leechers = 0;
	int seeders = 0;
	int tor_user_id = 0;
	bool dirty = true;
	bool freeleech = false;
	float upload_multiplier;
	float download_multiplier;
};

struct t_user
{
	long long timespent_seeding = 0;
	bool can_leech = true;
	bool marked;
	bool freeleech = false;
	int uid;
	int peers_limit = 0;
	int torrent_pass_version = 0;
	int wait_time = 0;

	int at_up = 0; // Active Torrent (seeding)
	int at_down = 0; // Active Torrent (downloading)

	float upload_multiplier;
	float download_multiplier;
};

struct t_deny_from_host
{
	unsigned int end;
	bool marked;
};

const t_torrent* find_torrent(const std::string& id);
t_user* find_user_by_torrent_pass(str_ref, str_ref info_hash);
t_user* find_user_by_uid(int v);
long long srv_secret();
const Cconfig& srv_config();
Cdatabase& srv_database();
Cstats& srv_stats();
time_t srv_time();

int srv_run(const std::string& table_prefix, bool use_sql, const std::string& conf_file);
void srv_term();
std::string srv_debug(const Ctracker_input&);
std::string srv_insert_peer(const Ctracker_input&, bool udp, t_user*);
std::string srv_scrape(const Ctracker_input&, t_user*);
std::string srv_select_peers(const Ctracker_input&);
std::string srv_statistics();
std::string srv_update_tracker(boost::unordered_map<std::string, std::string>&);
std::string write_json(const boost::unordered_map<std::string, std::string>&);
std::string write_json(const std::string& str);
