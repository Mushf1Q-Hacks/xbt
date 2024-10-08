#include "stdafx.h"
#include "server.h"

#include <bt_strings.h>
#include "connection.h"
#include "epoll.h"
#include "transaction.h"

using namespace std;

namespace std
{
	template<class T, size_t N>
	struct hash<array<T, N>>
	{
		size_t operator()(const array<T, N>& v) const
		{
			return boost::hash_range(v.begin(), v.end());
		}
	};
}

static volatile bool g_sig_term = false;
static boost::ptr_list<Cconnection> g_connections;
static unordered_map<array<char, 20>, t_torrent> g_torrents;
static unordered_map<int, t_user> g_users;
static unordered_map<array<char, 32>, t_user*> g_users_torrent_passes;
static Cconfig g_config;
static Cdatabase g_database;
static Cepoll g_epoll;
static Cstats g_stats;
static string g_announce_log_buffer;
static string g_conf_file;
static string g_scrape_log_buffer;
static string g_table_prefix;
static string g_torrents_users_updates_buffer;
static string g_users_updates_buffer;
static time_t g_clean_up_time;
static time_t g_read_config_time;
static time_t g_read_db_torrents_time;
static time_t g_read_db_users_time;
static time_t g_time = time(NULL);
static time_t g_write_db_torrents_time;
static time_t g_write_db_users_time;
static unsigned long long g_secret;
static int g_fid_end = 0;
static bool g_read_users_can_leech;
static bool g_read_users_peers_limit;
static bool g_read_users_torrent_pass;
static bool g_read_users_wait_time;
static bool g_use_sql;

/* Aayush */
static time_t m_read_db_deny_from_clients_time;
static time_t m_read_db_deny_from_hosts_time;

static bool g_read_users_freeleech;

static map<unsigned int, t_deny_from_host> m_deny_from_hosts;
static vector<string> m_deny_from_clients;

static string m_snatched_buffer;
static string m_cheat_log_buffer;
// static string m_active_torrent_buffer;
/* Aayush */

void accept(const Csocket&);

static void async_query(const string& v)
{
	g_database.query_nothrow(v);
}

static void sig_handler(int v)
{
	if (v == SIGTERM)
		g_sig_term = true;
}

class Ctcp_listen_socket : public Cclient
{
public:
	Ctcp_listen_socket(const Csocket& s)
	{
		m_s = s;
	}

	virtual void process_events(int)
	{
		accept(m_s);
	}
};

class Cudp_listen_socket : public Cclient
{
public:
	Cudp_listen_socket(const Csocket& s)
	{
		m_s = s;
	}

	virtual void process_events(int events)
	{
		if (events & EPOLLIN)
			Ctransaction(m_s).recv();
	}
};

const Cconfig& srv_config()
{
	return g_config;
}

Cdatabase& srv_database()
{
	return g_database;
}

const t_torrent* find_torrent(const string& id)
{
	return find_ptr(g_torrents, to_array<char, 20>(id));
}

t_user* find_user_by_uid(int v)
{
	return find_ptr(g_users, v);
}

long long srv_secret()
{
	return g_secret;
}

Cstats& srv_stats()
{
	return g_stats;
}

time_t srv_time()
{
	return g_time;
}

static bool banned_client(const array<char, 20>& m_peer_id)
{
	string peer_id(begin(m_peer_id), end(m_peer_id));
	for (unsigned int i = 0; i < m_deny_from_clients.size(); i++) {
		if (peer_id.compare(0, m_deny_from_clients[i].length(), m_deny_from_clients[i]) == 0) {
			return true;
		}
	}
	
	return false;
}

void read_config()
{
	if (g_use_sql)
	{
		try
		{
			Cconfig config;
			for (auto row : Csql_query(g_database, "select name, value from @config where value is not null").execute())
			{
				if (config.set(row[0].s(), row[1].s()))
					cerr << "unknown config name: " << row[0].s() << endl;
			}
			config.load(g_conf_file);
			if (config.m_torrent_pass_private_key.empty())
			{
				config.m_torrent_pass_private_key = generate_random_string(27);
				Csql_query(g_database, "insert into @config (name, value) values ('torrent_pass_private_key', ?)")(config.m_torrent_pass_private_key).execute();
			}
			g_config = config;
			g_database.set_name("completed", g_config.m_column_files_completed);
			g_database.set_name("leechers", g_config.m_column_files_leechers);
			g_database.set_name("seeders", g_config.m_column_files_seeders);
			g_database.set_name("fid", g_config.m_column_files_fid);
			g_database.set_name("uid", g_config.m_column_users_uid);
			g_database.set_name("announce_log", g_config.m_table_announce_log.empty() ? g_table_prefix + "announce_log" : g_config.m_table_announce_log);
			g_database.set_name("files", g_config.m_table_torrents.empty() ? g_table_prefix + "files" : g_config.m_table_torrents);
			g_database.set_name("files_users", g_config.m_table_torrents_users.empty() ? g_table_prefix + "files_users" : g_config.m_table_torrents_users);
			g_database.set_name("scrape_log", g_config.m_table_scrape_log.empty() ? g_table_prefix + "scrape_log" : g_config.m_table_scrape_log);
			g_database.set_name("users", g_config.m_table_users.empty() ? g_table_prefix + "users" : g_config.m_table_users);
			g_database.set_name("deny_from_clients", g_table_prefix + "deny_from_clients");
			g_database.set_name("deny_from_hosts", g_table_prefix + "deny_from_hosts");
			g_database.set_name("cheat_log", g_table_prefix + "cheat_log");
			g_database.set_name("snatched", g_table_prefix + "snatched");
		}
		catch (bad_query&)
		{
		}
	}
	else
	{
		Cconfig config;
		if (!config.load(g_conf_file))
			g_config = config;
	}
	if (g_config.m_listen_ipas.empty())
		g_config.m_listen_ipas.insert(htonl(INADDR_ANY));
	if (g_config.m_listen_ports.empty())
		g_config.m_listen_ports.insert(2710);
	g_read_config_time = srv_time();
}

/* @Aayush */
void read_db_torrents_sql()
{
	try
	{
		if (!g_config.m_auto_register)
        {
        	string buffer;

            Csql_result result = Csql_query(g_database, "select info_hash, @fid, flags, freeleech from @files where flags & 1").execute();
            while (Csql_row row = result.fetch_row())
            {
            	if (row[2].i() == 1)
            	{
                	g_torrents.erase(to_array<char, 20>(row[0]));
                	Csql_query(g_database, "delete from @files where @fid = ?")(row[1]).execute();
            	}
            	else
            	{
            		if (find_torrent(row[0].s())) {
            			t_torrent& file = g_torrents[to_array<char, 20>(row[0])];
            			file.freeleech = (row[3].i() ? true : false);
            		}

            		buffer += row[1].s() + ",";
            	}
            }

            if (!buffer.empty())
            {
            	buffer.pop_back();
            	Csql_query(g_database, "update @files set flags = 0 where @fid IN (" + buffer + ")").execute();
            }
        }

        /*
        	XFDev Multiplier Implementation
        */

        {
			Csql_result result = Csql_query(g_database, "select info_hash, @fid, download_multiplier, upload_multiplier from @files where flags & 2").execute();
			while (Csql_row row = result.fetch_row())
			{
				if (t_torrent* i = find_ptr(g_torrents, to_array<char, 20>(row[0])))
				{
					i->download_multiplier = row[2].f();
					i->upload_multiplier = row[3].f();
				}
				Csql_query(g_database, "update @files set flags = flags & ~2 where @fid = ?")(row[1].i()).execute();
			}
		}

		/*
        	XFDev Multiplier Implementation Ends
        */

		if (g_config.m_auto_register && !g_torrents.empty())
			return;
		for (auto row : Csql_query(g_database, "select info_hash, @completed, @fid, ctime, download_multiplier, upload_multiplier, freeleech, @uid from @files where @fid >= ?")(g_fid_end).execute())
		{
			g_fid_end = max<int>(g_fid_end, row[2].i() + 1);
			if (row[0].size() != 20 || find_torrent(row[0].s()))
				continue;
			t_torrent& file = g_torrents[to_array<char, 20>(row[0])];
			if (file.fid)
				continue;
			file.completed = row[1].i();
			file.dirty = false;
			file.fid = row[2].i();
			file.ctime = row[3].i();
			file.download_multiplier = row[4].f();
			file.upload_multiplier = row[5].f();
			file.freeleech = (row[6].i() ? true : false);
			file.tor_user_id = row[7].i();

		}
	}
	catch (bad_query&)
	{
	}
}
/* @Aayush */

void read_db_torrents()
{
	g_read_db_torrents_time = srv_time();
	if (g_use_sql)
		read_db_torrents_sql();
	else if (!g_config.m_auto_register)
    {
        set<t_torrent*> new_torrents;
        ifstream is("xbt_torrents.txt");
        for (string s; getline(is, s); )
        {
            s = hex_decode(s);
            if (s.size() == 20)
                new_torrents.insert(&g_torrents[to_array<char, 20>(s)]);
        }
        for (auto i = g_torrents.begin(); i != g_torrents.end(); )
        {
            if (new_torrents.count(&i->second))
                i++;
            else
               	g_torrents.erase(i++);
        }
    }
}

/* @Aayush */
void read_db_deny_from_clients()
{
	m_read_db_deny_from_clients_time = srv_time();

	if (!g_use_sql)
		return;
	try
	{
		Csql_result result = Csql_query(g_database, "select peer_id from @deny_from_clients").execute();
		m_deny_from_clients.clear();

		while (Csql_row row = result.fetch_row())
			m_deny_from_clients.push_back(row[0].s());
	}
	catch (bad_query&)
	{
	}
}

void read_db_deny_from_hosts()
{
	m_read_db_deny_from_hosts_time = srv_time();
	if (!g_use_sql)
		return;
	
	try
	{
		Csql_result result = Csql_query(g_database, "select begin, end from @deny_from_hosts").execute();
		for (auto i = m_deny_from_hosts.begin(); i != m_deny_from_hosts.end(); i++)
			i->second.marked = true;
		
		while (Csql_row row = result.fetch_row())
		{
			t_deny_from_host& deny_from_host = m_deny_from_hosts[row[0].i()];
			deny_from_host.marked = false;
			deny_from_host.end = row[1].i();
		}

		for (auto i = m_deny_from_hosts.begin(); i != m_deny_from_hosts.end(); )
		{
			if (i->second.marked)
				m_deny_from_hosts.erase(i++);
			else
				i++;
		}
	}
	catch (bad_query&)
	{
	}
}
/* @Aayush */

void read_db_users()
{
	g_read_db_users_time = srv_time();
	if (!g_use_sql)
		return;
	try
	{
		Csql_query q(g_database, "select @uid, download_multiplier, upload_multiplier");
		if (g_read_users_can_leech)
			q += ", can_leech";
		if (g_read_users_peers_limit)
			q += ", peers_limit";
		if (g_read_users_torrent_pass)
			q += ", torrent_pass";
		q += ", torrent_pass_version";
		if (g_read_users_wait_time)
			q += ", wait_time";
		if (g_read_users_freeleech)
			q += ", freeleech";
		q += " from @users";
		Csql_result result = q.execute();
		g_users.reserve(result.size());
		for (auto& i : g_users)
			i.second.marked = true;
		g_users_torrent_passes.clear();
		for (auto row : result)
		{
			t_user& user = g_users[row[0].i()];
			user.marked = false;
			int c = 0;
			user.uid = row[c++].i();
			user.download_multiplier = row[c++].f();
			user.upload_multiplier = row[c++].f();
			if (g_read_users_can_leech)
				user.can_leech = row[c++].i();
			if (g_read_users_peers_limit)
				user.peers_limit = row[c++].i();
			if (g_read_users_torrent_pass)
			{
				if (row[c].size() == 32)
					g_users_torrent_passes[to_array<char, 32>(row[c])] = &user;
				c++;
			}
			user.torrent_pass_version = row[c++].i();
			if (g_read_users_wait_time)
				user.wait_time = row[c++].i();
			if (g_read_users_freeleech)
				user.freeleech = (row[c++].i() ? true : false);
		}
		for (auto i = g_users.begin(); i != g_users.end(); )
		{
			if (i->second.marked)
				g_users.erase(i++);
			else
				i++;
		}
	}
	catch (bad_query&)
	{
	}
}
/* @Aayush */

const string& db_name(const string& v)
{
	return g_database.name(v);
}

void write_db_torrents()
{
	g_write_db_torrents_time = srv_time();
	if (!g_use_sql)
		return;
	try
	{
		string buffer;
		while (1)
		{
			for (auto& i : g_torrents)
			{
				t_torrent& file = i.second;
				if (!file.dirty || !file.fid)
					continue;				
				buffer += Csql_query(g_database, "(?,?,?,?),")(file.leechers)(file.seeders)(file.completed)(file.fid).read();
				file.dirty = false;
				if (buffer.size() > 255 << 10)
					break;
			}
			if (buffer.empty())
				break;
			buffer.pop_back();
			async_query("insert into " + db_name("files") + " (" + db_name("leechers") + ", " + db_name("seeders") + ", " + db_name("completed") + ", " + db_name("fid") + ") values "
				+ buffer
				+ " on duplicate key update"
				+ "  " + db_name("leechers") + " = values(" + db_name("leechers") + "),"
				+ "  " + db_name("seeders") + " = values(" + db_name("seeders") + "),"
				+ "  " + db_name("completed") + " = values(" + db_name("completed") + "),"
				+ "  mtime = unix_timestamp()");
			buffer.clear();
			cout << "Flushed torrents" << endl;
		}
	}
	catch (bad_query&)
	{
	}
	if (!g_announce_log_buffer.empty())
	{
		g_announce_log_buffer.pop_back();
		async_query("insert delayed into " + db_name("announce_log") + " (ipa, port, event, info_hash, peer_id, downloaded, left0, uploaded, uid, mtime) values " + g_announce_log_buffer);
		g_announce_log_buffer.erase();
	}
	if (!g_scrape_log_buffer.empty())
	{
		g_scrape_log_buffer.pop_back();
		async_query("insert delayed into " + db_name("scrape_log") + " (ipa, uid, mtime) values " + g_scrape_log_buffer);
		g_scrape_log_buffer.erase();
	}
}

void write_db_users()
{
	g_write_db_users_time = srv_time();
	if (!g_use_sql)
		return;
	if (!g_torrents_users_updates_buffer.empty())
	{
		g_torrents_users_updates_buffer.pop_back();
		async_query("insert into " + db_name("files_users") + " (active, announced, completed, downloaded, `left`, uploaded, mtime, " + db_name("fid") + ", " +  db_name("uid") + ", down_rate, up_rate, useragent, leechtime, seedtime, peer_id, ipa) values "
			+ g_torrents_users_updates_buffer
			+ " on duplicate key update"
			+ "  active = values(active),"
			+ "  announced = announced + values(announced),"
			+ "  completed = completed + values(completed),"
			+ "  downloaded = downloaded + values(downloaded),"
			+ "  `left` = values(`left`),"
			+ "  uploaded = uploaded + values(uploaded),"
			+ "  mtime = values(mtime),"
			+ "  down_rate = values(down_rate),"
			+ "  up_rate = values(up_rate),"
			+ "  useragent = values(useragent),"
			+ "  leechtime = leechtime + values(leechtime),"
			+ "  seedtime = seedtime + values(seedtime),"
			+ "  peer_id = values(peer_id),"
			+ "  ipa = values(ipa)");
		g_torrents_users_updates_buffer.erase();
		cout << "Flushed peers" << endl;
	}

	if (!m_snatched_buffer.empty())
	{
		m_snatched_buffer.pop_back();
		async_query("insert into " + db_name("snatched") + " (" + db_name("uid") + ", mtime, " + db_name("fid") + ", ipa) values "
			+ m_snatched_buffer);
		m_snatched_buffer.erase();
		cout << "Flushed snatches" << endl;
	}

	// if (!m_active_torrent_buffer.empty())
	// {
	// 	m_active_torrent_buffer.pop_back();
	// 	async_query("insert into xftt_active_torrent (" + db_name("uid") + ", up, down) values "
	// 		+ m_active_torrent_buffer
	// 		+ " on duplicate key update"
	// 		+ " up = values(up),"
	// 		+ " down = values(down)"
	// 	);
	// 	m_active_torrent_buffer.erase();
	// }

	//async_query("update " + db_name("files_users") + " set active = 0 where mtime < unix_timestamp() - 60 * 60");
	
	if (!g_users_updates_buffer.empty())
	{
		g_users_updates_buffer.pop_back();
		async_query("insert into " + db_name("users") + " (downloaded, uploaded, " + db_name("uid") + ", seedbonus) values "
			+ g_users_updates_buffer
			+ " on duplicate key update"
			+ "  downloaded = downloaded + values(downloaded),"
			+ "  uploaded = uploaded + values(uploaded),"
			+ "  seedbonus = seedbonus + values(seedbonus)");
		g_users_updates_buffer.erase();
		cout << "Flushed users" << endl;
	}
}

int test_sql()
{
	if (!g_use_sql)
		return 0;
	try
	{
		mysql_get_server_version(g_database);
		if (g_config.m_log_announce)
			Csql_query(g_database, "select id, ipa, port, event, info_hash, peer_id, downloaded, left0, uploaded, uid, mtime from @announce_log where 0").execute();
		Csql_query(g_database, "select name, value from @config where 0").execute();
		Csql_query(g_database, "select @fid, info_hash, @leechers, @seeders, flags, mtime, ctime, freeleech, @uid from @files where 0").execute();
		Csql_query(g_database, "select @fid, @uid, active, announced, completed, downloaded, `left`, uploaded, up_rate, down_rate, useragent, peer_id, ipa, seedtime, leechtime from @files_users where 0").execute();
		if (g_config.m_log_scrape)
			Csql_query(g_database, "select id, ipa, uid, mtime from @scrape_log where 0").execute();
		Csql_query(g_database, "select @uid, torrent_pass_version, downloaded, uploaded, seedbonus from @users where 0").execute();
		Csql_query(g_database, "select @uid, @fid, mtime, ipa from @snatched where 0").execute();
		Csql_query(g_database, "update @files set @leechers = 0, @seeders = 0").execute();
		// Csql_query(g_database, "update @files_users set active = 0").execute();
		g_read_users_can_leech = Csql_query(g_database, "show columns from @users like 'can_leech'").execute();
		g_read_users_peers_limit = Csql_query(g_database, "show columns from @users like 'peers_limit'").execute();
		g_read_users_torrent_pass = Csql_query(g_database, "show columns from @users like 'torrent_pass'").execute();
		g_read_users_wait_time = Csql_query(g_database, "show columns from @users like 'wait_time'").execute();
		g_read_users_freeleech = Csql_query(g_database, "show columns from @users like 'freeleech'").execute();
		return 0;
	}
	catch (bad_query&)
	{
	}
	return 1;
}

void clean_up(t_torrent& t, time_t time)
{
    for (auto i = t.peers.begin(); i != t.peers.end(); )
    {
        if (i->second.mtime < time)
        {
            (i->second.left ? t.leechers : t.seeders)--;
            t.peers.erase(i++);
            t.dirty = true;
        }
        else
        	i++;
    }
}

void clean_up()
{
	for (auto& i : g_torrents)
		clean_up(i.second, srv_time() - static_cast<int>(1.5 * g_config.m_announce_interval));
	g_clean_up_time = srv_time();
}

/* @Aayush */
void clean_peer_data()
{
	try {
		Csql_query(g_database, "TRUNCATE @files_users").execute();
	}
	catch(bad_query&) {
		cerr << "Unable to truncate peers table!" << endl;
	}
}
/* @Aayush */

int srv_run(const string& table_prefix, bool use_sql, const string& conf_file)
{
	for (int i = 0; i < 8; i++)
		g_secret = g_secret << 8 ^ rand();
	g_conf_file = conf_file;
	g_database.set_name("config", table_prefix + "config");
	g_table_prefix = table_prefix;
	g_use_sql = use_sql;

	read_config();
	if (test_sql())
		return 1;
	if (g_epoll.create() == -1)
	{
		cerr << "epoll_create failed" << endl;
		return 1;
	}
	list<Ctcp_listen_socket> lt;
	list<Cudp_listen_socket> lu;
	for (auto& j : g_config.m_listen_ipas)
	{
		for (auto& i : g_config.m_listen_ports)
		{
			Csocket l;
			if (l.open(SOCK_STREAM) == INVALID_SOCKET)
				cerr << "socket failed: " << Csocket::error2a(WSAGetLastError()) << endl;
			else if (l.setsockopt(SOL_SOCKET, SO_REUSEADDR, true),
				l.bind(j, htons(i)))
				cerr << "bind failed: " << Csocket::error2a(WSAGetLastError()) << endl;
			else if (l.listen())
				cerr << "listen failed: " << Csocket::error2a(WSAGetLastError()) << endl;
			else
			{
#ifdef SO_ACCEPTFILTER
				accept_filter_arg afa;
				bzero(&afa, sizeof(afa));
				strcpy(afa.af_name, "httpready");
				if (l.setsockopt(SOL_SOCKET, SO_ACCEPTFILTER, &afa, sizeof(afa)))
					cerr << "setsockopt failed: " << Csocket::error2a(WSAGetLastError()) << endl;
#elif 0 // TCP_DEFER_ACCEPT
				if (l.setsockopt(IPPROTO_TCP, TCP_DEFER_ACCEPT, 90))
					cerr << "setsockopt failed: " << Csocket::error2a(WSAGetLastError()) << endl;
#endif
				lt.push_back(Ctcp_listen_socket(l));
				if (!g_epoll.ctl(EPOLL_CTL_ADD, l, EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLERR | EPOLLHUP, &lt.back()))
					continue;
			}
			return 1;
		}
		for (auto& i : g_config.m_listen_ports)
		{
			Csocket l;
			if (l.open(SOCK_DGRAM) == INVALID_SOCKET)
				cerr << "socket failed: " << Csocket::error2a(WSAGetLastError()) << endl;
			else if (l.setsockopt(SOL_SOCKET, SO_REUSEADDR, true),
				l.bind(j, htons(i)))
				cerr << "bind failed: " << Csocket::error2a(WSAGetLastError()) << endl;
			else
			{
				lu.push_back(Cudp_listen_socket(l));
				if (!g_epoll.ctl(EPOLL_CTL_ADD, l, EPOLLIN | EPOLLPRI | EPOLLERR | EPOLLHUP, &lu.back()))
					continue;
			}
			return 1;
		}
	}

	//clean_peer_data(); // @Aayush
	clean_up();
	read_db_deny_from_hosts(); // @Aayush
	read_db_deny_from_clients(); // @Aayush
	read_db_torrents();
	read_db_users();
	write_db_torrents();
	write_db_users();
#ifndef NDEBUG
	// test_announce();
#endif
#ifndef WIN32
	if (g_config.m_daemon)
	{
		if (daemon(true, false))
			cerr << "daemon failed" << endl;
		ofstream(g_config.m_pid_file.c_str()) << getpid() << endl;
		struct sigaction act;
		act.sa_handler = sig_handler;
		sigemptyset(&act.sa_mask);
		act.sa_flags = 0;
		if (sigaction(SIGTERM, &act, NULL))
			cerr << "sigaction failed" << endl;
		act.sa_handler = SIG_IGN;
		if (sigaction(SIGPIPE, &act, NULL))
			cerr << "sigaction failed" << endl;
	}
#endif
#ifdef EPOLL
	array<epoll_event, 64> events;
#else
	fd_set fd_read_set;
	fd_set fd_write_set;
	fd_set fd_except_set;
#endif
	while (!g_sig_term)
	{
#ifdef EPOLL
		int r = g_epoll.wait(events.data(), events.size(), 5000);
		if (r == -1)
			cerr << "epoll_wait failed: " << errno << endl;
		else
		{
			time_t prev_time = g_time;
			g_time = ::time(NULL);
			for (int i = 0; i < r; i++)
				reinterpret_cast<Cclient*>(events[i].data.ptr)->process_events(events[i].events);
			if (g_time == prev_time)
				continue;
			for (auto i = g_connections.begin(); i != g_connections.end(); )
			{
				if (i->run())
					i = g_connections.erase(i);
				else
					i++;
			}
		}
#else
		FD_ZERO(&fd_read_set);
		FD_ZERO(&fd_write_set);
		FD_ZERO(&fd_except_set);
		int n = 0;
		for (auto& i : g_connections)
		{
			int z = i.pre_select(&fd_read_set, &fd_write_set);
			n = max(n, z);
		}
		for (auto& i : lt)
		{
			FD_SET(i.s(), &fd_read_set);
			n = max<int>(n, i.s());
		}
		for (auto& i : lu)
		{
			FD_SET(i.s(), &fd_read_set);
			n = max<int>(n, i.s());
		}
		timeval tv;
		tv.tv_sec = 5;
		tv.tv_usec = 0;
		if (select(n + 1, &fd_read_set, &fd_write_set, &fd_except_set, &tv) == SOCKET_ERROR)
			cerr << "select failed: " << Csocket::error2a(WSAGetLastError()) << endl;
		else
		{
			g_time = ::time(NULL);
			for (auto& i : lt)
			{
				if (FD_ISSET(i.s(), &fd_read_set))
					accept(i.s());
			}
			for (auto& i : lu)
			{
				if (FD_ISSET(i.s(), &fd_read_set))
					Ctransaction(i.s()).recv();
			}
			for (auto i = g_connections.begin(); i != g_connections.end(); )
			{
				if (i->post_select(&fd_read_set, &fd_write_set))
					i = g_connections.erase(i);
				else
					i++;
			}
		}
#endif
		if (srv_time() - g_read_config_time > g_config.m_read_config_interval)
			read_config();
		else if (srv_time() - g_clean_up_time > g_config.m_clean_up_interval)
			clean_up();
		else if (srv_time() - g_read_db_torrents_time > g_config.m_read_db_files_interval)
			read_db_torrents();
		else if (srv_time() - g_read_db_users_time > g_config.m_read_db_users_interval) 
			read_db_users();
		else if (srv_time() - m_read_db_deny_from_hosts_time > g_config.m_read_db_interval)
			read_db_deny_from_hosts();
		else if (srv_time() - m_read_db_deny_from_clients_time > g_config.m_read_db_interval)
			read_db_deny_from_clients();
		else if (g_config.m_write_db_interval && srv_time() - g_write_db_torrents_time > g_config.m_write_db_interval)
			write_db_torrents();
		else if (g_config.m_write_db_interval && srv_time() - g_write_db_users_time > g_config.m_write_db_interval)
			write_db_users();
	}
	write_db_torrents();
	write_db_users();
	unlink(g_config.m_pid_file.c_str());
	return 0;
}

void accept(const Csocket& l)
{
	sockaddr_in a;
	for (int z = 0; z < 10000; z++)
	{
		socklen_t cb_a = sizeof(sockaddr_in);
#ifdef SOCK_NONBLOCK
		Csocket s = accept4(l, reinterpret_cast<sockaddr*>(&a), &cb_a, SOCK_NONBLOCK);
#else
		Csocket s = ::accept(l, reinterpret_cast<sockaddr*>(&a), &cb_a);
#endif
		if (s == SOCKET_ERROR)
		{
			if (WSAGetLastError() == WSAECONNABORTED)
				continue;
			if (WSAGetLastError() != WSAEWOULDBLOCK)
			{
				g_stats.accept_errors++;
				cerr << "accept failed: " << Csocket::error2a(WSAGetLastError()) << endl;
				xbt_syslog("accept failed: " + Csocket::error2a(WSAGetLastError()));
			}
			break;
		}

		/* @Aayush */
		auto i = m_deny_from_hosts.lower_bound(ntohl(a.sin_addr.s_addr));
		if (i != m_deny_from_hosts.begin())
		{
			i--;
			if (ntohl(a.sin_addr.s_addr) <= i->second.end) 
			{
				g_stats.rejected_tcp++;
				continue;
			}
		}
		/* @Aayush */

		g_stats.accepted_tcp++;
#ifndef SOCK_NONBLOCK
		if (s.blocking(false))
			cerr << "ioctlsocket failed: " << Csocket::error2a(WSAGetLastError()) << endl;
#endif
		unique_ptr<Cconnection> connection(new Cconnection(s, a));
		connection->process_events(EPOLLIN);
		if (connection->s() != INVALID_SOCKET)
		{
			g_stats.slow_tcp++;
			g_connections.push_back(connection.release());
			g_epoll.ctl(EPOLL_CTL_ADD, g_connections.back().s(), EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLERR | EPOLLHUP | EPOLLET, &g_connections.back());
		}
	}
}

string srv_insert_peer(const Ctracker_input& v, bool udp, t_user* user)
{
	if (g_use_sql && g_config.m_log_announce)
	{
		g_announce_log_buffer += Csql_query(g_database, "(?,?,?,?,?,?,?,?,?,?),")
			(ntohl(v.m_ipa))
			(ntohs(v.m_port))
			(v.m_event)
			(v.m_info_hash)
			(v.m_peer_id)
			(v.m_downloaded)
			(v.m_left)
			(v.m_uploaded)
			(user ? user->uid : 0)
			(srv_time())
			.read();
	}
	if (!g_config.m_offline_message.empty())
		return g_config.m_offline_message;

	/* @Aayush */
	if (banned_client(v.m_peer_id))
		return bts_banned_client;
	if (!g_config.m_anonymous_announce && !user)
		return bts_unregistered_torrent_pass;
	if (!g_config.m_auto_register && !find_torrent(v.m_info_hash))
		return bts_unregistered_torrent;
	if (v.m_left && user && !user->can_leech)
		return bts_can_not_leech;
	t_torrent& file = g_torrents[to_array<char, 20>(v.m_info_hash)];
	if (!file.ctime)
		file.ctime = srv_time();
	if (v.m_left && user && user->wait_time && file.download_multiplier && file.ctime + user->wait_time > srv_time())
		return bts_wait_time;
	peer_key_c peer_key(v.m_ipa, user ? user->uid : 0);
	t_peer* i = find_ptr(file.peers, peer_key);

	bool completed_torrent = false;
	bool event_completed = (v.m_event == Ctracker_input::e_completed);
	bool event_notstop = (v.m_event != Ctracker_input::e_stopped);
	bool update_at = false;

	if (i)
	{
		// Min Request Interval: 10 seconds
		/*if (user 
			&& i->uid == user->uid 
			&& event_notstop
			&& i->mtime + 10 > srv_time()) {
			return bts_min_request_interval + " 10 seconds";
		}*/

		(i->left ? file.leechers : file.seeders)--;
		completed_torrent = (!v.m_left && event_completed); 

		if (!event_notstop && user) {
			(i->left ? user->at_down : user->at_up)--;
		}
	}
	else if(user)
	{
		if (v.m_left && user->peers_limit)
		{
			int c = 0;
			for (auto& j : file.peers)
				c += (j.second.uid == user->uid && j.second.left);

			if (c >= user->peers_limit)
				return bts_peers_limit_reached;
		}

		(event_notstop && v.m_left ? user->at_down : user->at_up)++;
		update_at = true;
	}

	if (g_use_sql && user && file.fid)
	{	
		long long downloaded = 0;
		long long uploaded = 0;

		/* @Aayush */
		long long timespent = 0;	
		long long upspeed = 0;
		long long downspeed = 0;
		long long seedbonus = 0;
		long long seedtime = 0;
		long long leechtime = 0;
		/* @Aayush */

		if (i
			&& i->uid == user->uid
			&& boost::equals(i->peer_id, v.m_peer_id)
			&& v.m_downloaded >= i->downloaded
			&& v.m_uploaded >= i->uploaded)
		{
			downloaded = v.m_downloaded - i->downloaded;
			uploaded = v.m_uploaded - i->uploaded;

			/* @Aayush */
			timespent = srv_time() - i->mtime;

			// Calculate up/down speed
			if (timespent > 0) // downloaded || uploaded)
			{
				upspeed = uploaded / timespent;
				downspeed = downloaded / timespent;
			}

			if (!v.m_left && !i->left)
			{
				// its a Seeder
				if (g_config.m_seedbonus) {
					user->timespent_seeding += timespent;
				}

				seedtime = timespent;
			}
			else if(v.m_left) {
				leechtime = timespent;
			}
			else if(event_completed)
			{
				leechtime = timespent;

				if (i->left)
				{
					user->at_up++;
					user->at_down--;

					update_at = true;
				}
			}
			/* @Aayush */
		}

		g_torrents_users_updates_buffer += Csql_query(g_database, "(?,1,?,?,?,?,?,?,?,?,?,?,?,?,?,?),")
			(event_notstop)
			(completed_torrent)
			(downloaded)
			(v.m_left)
			(uploaded)
			(srv_time())
			(file.fid)
			(user->uid)
			(downspeed)	
			(upspeed)	
			(v.m_agent)	
			(leechtime)	
			(seedtime)
			(v.m_peer_id)
			(ntohl(v.m_ipa))	
			.read();

		// if (update_at) {
		// 	m_active_torrent_buffer += Csql_query(g_database, "(?, ?, ?),")
		// 		(user->uid)
		// 		(user->at_up < 0 ? 0 : user->at_up)
		// 		(user->at_down < 0 ? 0 : user->at_down)
		// 		.read();
		// }
			
		/* @Aayush */ 
		if (g_config.m_freeleech || file.freeleech || user->freeleech)	{
			downloaded = 0; // Freeleech
		}

		// Bonus Point System //
		if (g_config.m_seedbonus && user->timespent_seeding >= g_config.m_seedbonus_interval) {
			seedbonus = g_config.m_seedbonus * (user->timespent_seeding / g_config.m_seedbonus_interval);
			user->timespent_seeding = 0;
		}

		/*Global Multiplier @XenDev*/
		if(g_config.m_global_multiplier)
		{
			if(downloaded || uploaded || seedbonus)
			{
				g_users_updates_buffer += Csql_query(g_database, "(?,?,?,?),")
				.p(downloaded * g_config.m_download_multiplier)
				.p(uploaded * g_config.m_upload_multiplier)
				.p(user->uid)
				.p(seedbonus)
				.read();
			}
		}else {

			if(downloaded || uploaded || seedbonus) {
				g_users_updates_buffer += Csql_query(g_database, "(?,?,?,?),")
				.p(downloaded * file.download_multiplier * user->download_multiplier)
				.p(uploaded * file.upload_multiplier * user->upload_multiplier)
				.p(user->uid)
				.p(seedbonus)
				.read();
			}
		}

		if (completed_torrent)
		{
			m_snatched_buffer += Csql_query(g_database, "(?, ?, ?, ?),")
				(user->uid)
				(srv_time())
				(file.fid)
				(ntohl(v.m_ipa))
				.read();
		}
		/* @Aayush */

		if (g_torrents_users_updates_buffer.size() > 255 << 10)
			write_db_users();
	}

	if (!event_notstop)
		file.peers.erase(peer_key);
	else
	{
		t_peer& peer = i ? *i : file.peers[peer_key];
		peer.downloaded = v.m_downloaded;
		peer.left = v.m_left;
		peer.peer_id = v.m_peer_id;
		peer.port = v.m_port;
		peer.uid = user ? user->uid : 0;
		peer.uploaded = v.m_uploaded;
		(peer.left ? file.leechers : file.seeders)++;
		
		peer.mtime = srv_time();
	}

	/* @Aayush */
	if (completed_torrent) 
		file.completed++;

	(udp ? g_stats.announced_udp : g_stats.announced_http)++;
	file.dirty = true;
	return string();
}

void t_torrent::select_peers(mutable_str_ref& d, const Ctracker_input& ti) const
{
	if (ti.m_event == Ctracker_input::e_stopped)
		return;
	vector<array<char, 6>> candidates;
	candidates.reserve(peers.size());
	for (auto& i : peers)
	{
		if (!ti.m_left && !i.second.left)
			continue;
		array<char, 6> v;
		memcpy(&v[0], &i.first.host_, 4);
		memcpy(&v[4], &i.second.port, 2);
		candidates.push_back(v);
	}
	size_t c = d.size() / 6;
	
	/* @Aayush */
	// size_t c = (ti.m_num_want <= 0 ? 50 : min(ti.m_num_want, 50));

	if (candidates.size() <= c)
	{
		memcpy(d.data(), candidates);
		d.advance_begin(6 * candidates.size());
		return;
	}
	while (c--)
	{
		int i = rand() % candidates.size();
		memcpy(d.data(), candidates[i]);
		d.advance_begin(6);
		candidates[i] = candidates.back();
		candidates.pop_back();
	}
}

string srv_select_peers(const Ctracker_input& ti)
{
    const t_torrent* f = find_torrent(ti.m_info_hash);
    if (!f)
            return string();
    array<char, 300> peers0;
		mutable_str_ref peers = peers0;
		f->select_peers(peers, ti);
		peers.assign(peers0.data(), peers.data());
		return (boost::format("d8:completei%de10:incompletei%de8:intervali%de12:min intervali%de5:peers%d:%se")
			% f->seeders % f->leechers % g_config.m_announce_interval % g_config.m_announce_interval % peers.size() % peers).str();
}

/* @Aayush */
string srv_update_tracker(boost::unordered_map<string, string> &params)
{		
	if (params["key"] != g_config.m_torrent_pass_private_key)
	{
		return write_json("Authentication failure");
	}

	if (params["action"] == "status") 
	{
		return write_json("online");
	}
	else if(params["action"] == "stats") 
	{
		boost::unordered_map<string, string> params;
		long long seeders = 0;
		long long leechers = 0;
		long long snatches = 0;
		long long torrents = 0;

		for (auto& i : g_torrents)
		{
			seeders += i.second.seeders;
			leechers += i.second.leechers;
			snatches += i.second.completed;
			torrents++;
		}

		params.emplace("seeders", to_string(seeders));
		params.emplace("leechers", to_string(leechers));
		params.emplace("torrents", to_string(torrents));
		params.emplace("snatches", to_string(snatches));

		return write_json(params);
	}
	else
	{
		return write_json("Unknown action specified");
	}
}

string write_json(const string &message)
{
	boost::unordered_map<string, string> map;
	map.emplace("message", message);

	return write_json(map);
}

string write_json(const boost::unordered_map<string, string> &params)
{
	ostringstream os;
	os << "{";

	for (const auto &p : params)
		os << "\"" << p.first << "\"" << ":" << "\"" << p.second << "\"" << ",";

	string str = os.str();
	str.back() = '}';

	return str;
}

/* @Aayush */

string srv_scrape(const Ctracker_input& ti, t_user* user)
{
	if (g_use_sql && g_config.m_log_scrape)
		g_scrape_log_buffer += Csql_query(g_database, "(?,?,?),")(ntohl(ti.m_ipa))(user ? user->uid : 0)(srv_time()).read();
	if (!g_config.m_anonymous_scrape && !user)
		return "d14:failure reason25:unregistered torrent passe";
	string d;
	d += "d5:filesd";
	if (ti.m_info_hashes.empty())
	{
		g_stats.scraped_full++;
		d.reserve(90 * g_torrents.size());
		for (auto& i : g_torrents)
		{
			if (i.second.leechers || i.second.seeders)
				d += (boost::format("20:%sd8:completei%de10:downloadedi%de10:incompletei%dee") % boost::make_iterator_range(i.first) % i.second.seeders % i.second.completed % i.second.leechers).str();
		}
	}
	else
	{
		g_stats.scraped_http++;
		if (ti.m_info_hashes.size() > 1)
			g_stats.scraped_multi++;
		for (auto& j : ti.m_info_hashes)
		{
			if (const t_torrent* i = find_torrent(j))
				d += (boost::format("20:%sd8:completei%de10:downloadedi%de10:incompletei%dee") % j % i->seeders % i->completed % i->leechers).str();
		}
	}
	d += "e";
	if (g_config.m_scrape_interval)
		d += (boost::format("5:flagsd20:min_request_intervali%dee") % g_config.m_scrape_interval).str();
	d += "e";
	return d;
}

void debug(const t_torrent& t, string& os)
{
	for (auto& i : t.peers)
	{
		os << "<tr><td>" << Csocket::inet_ntoa(i.first.host_)
			<< "<td class=ar>" << ntohs(i.second.port)
			<< "<td class=ar>" << i.second.uid
			<< "<td class=ar>" << i.second.left
			<< "<td class=ar>" << srv_time() - i.second.mtime
			<< "<td>" << hex_encode(i.second.peer_id);
	}
}

string srv_debug(const Ctracker_input& ti)
{
	string os;
	os << "<!DOCTYPE HTML><meta http-equiv=refresh content=60><title>XBT Tracker</title>";
	os << "<style>.ar { text-align: right }</style>";
	os << "<table>";
	if (ti.m_info_hash.empty())
	{
		os << "<tr rowspan=2><td class=ar>" << ntohl(ti.m_ipa)
			<< "<td>" << ntohs(ti.m_port);
		for (auto& i : g_torrents)
		{
			if (!i.second.leechers && !i.second.seeders)
				continue;
			os << "<tr><td class=ar>" << i.second.fid
				<< "<td><a href=\"?info_hash=" << uri_encode(i.first) << "\">" << hex_encode(i.first) << "</a>"
				<< "<td>" << (i.second.dirty ? '*' : ' ')
				<< "<td class=ar>" << i.second.leechers
				<< "<td class=ar>" << i.second.seeders;
		}
	}
	else if (const t_torrent* i = find_torrent(ti.m_info_hash))
		debug(*i, os);
	os << "</table>";
	return os;
}

string srv_statistics()
{
    string os;
    os << "<!DOCTYPE HTML><meta http-equiv=refresh content=60><title>XBT Tracker</title>";
    os << "<style>.ar { text-align: right }</style>";
    long long leechers = 0;
    long long seeders = 0;
    int torrents = 0;
    for (auto& i : g_torrents)
    {
        leechers += i.second.leechers;
        seeders += i.second.seeders;
        torrents += i.second.leechers || i.second.seeders;
    }
    int peers = leechers + seeders;
    time_t t = srv_time();
    time_t up_time = max<time_t>(1, t - g_stats.start_time);
    os << "<table>"
            << "<tr><td>peers<td class=ar>" << peers;
    if (peers)
    {
            os << "<tr><td>seeders<td class=ar>" << seeders << "<td class=ar>" << seeders * 100 / peers << " %"
                    << "<tr><td>leechers<td class=ar>" << leechers << "<td class=ar>" << leechers * 100 / peers << " %";
    }
    os << "<tr><td>torrents<td class=ar>" << torrents
            << "<tr><td>"
            << "<tr><td>accepted tcp<td class=ar>" << g_stats.accepted_tcp << "<td class=ar>" << g_stats.accepted_tcp / up_time << " /s"
            << "<tr><td>slow tcp<td class=ar>" << g_stats.slow_tcp << "<td class=ar>" << g_stats.slow_tcp / up_time << " /s"
            << "<tr><td>rejected tcp<td class=ar>" << g_stats.rejected_tcp
            << "<tr><td>accept errors<td class=ar>" << g_stats.accept_errors;
    if (g_stats.announced())
    {
            os << "<tr><td>announced<td class=ar>" << g_stats.announced() << "<td class=ar>" << g_stats.announced() * 100 / g_stats.accepted_tcp << " %"
                    << "<tr><td>announced http <td class=ar>" << g_stats.announced_http << "<td class=ar>" << g_stats.announced_http * 100 / g_stats.announced() << " %"
                    << "<tr><td>announced udp<td class=ar>" << g_stats.announced_udp << "<td class=ar>" << g_stats.announced_udp * 100 / g_stats.announced() << " %";
    }
    os << "<tr><td>scraped full<td class=ar>" << g_stats.scraped_full;
    os << "<tr><td>scraped multi<td class=ar>" << g_stats.scraped_multi;
    if (g_stats.scraped())
    {
            os << "<tr><td>scraped<td class=ar>" << g_stats.scraped() << "<td class=ar>" << g_stats.scraped() * 100 / g_stats.accepted_tcp << " %"
                    << "<tr><td>scraped http<td class=ar>" << g_stats.scraped_http << "<td class=ar>" << g_stats.scraped_http * 100 / g_stats.scraped() << " %"
                    << "<tr><td>scraped udp<td class=ar>" << g_stats.scraped_udp << "<td class=ar>" << g_stats.scraped_udp * 100 / g_stats.scraped() << " %";
    }
    os << "<tr><td>"
            << "<tr><td>up time<td class=ar>" << duration2a(up_time)
            << "<tr><td>"
            << "<tr><td>anonymous announce<td class=ar>" << g_config.m_anonymous_announce
            << "<tr><td>anonymous scrape<td class=ar>" << g_config.m_anonymous_scrape
            << "<tr><td>auto register<td class=ar>" << g_config.m_auto_register
            << "<tr><td>full scrape<td class=ar>" << g_config.m_full_scrape
            << "<tr><td>read config time<td class=ar>" << t - g_read_config_time << " / " << g_config.m_read_config_interval
            << "<tr><td>clean up time<td class=ar>" << t - g_clean_up_time << " / " << g_config.m_clean_up_interval
            << "<tr><td>read db files time<td class=ar>" << t - g_read_db_torrents_time << " / " << g_config.m_read_db_files_interval;
    if (g_use_sql)
    {
            os << "<tr><td>read db users time<td class=ar>" << t - g_read_db_users_time << " / " << g_config.m_read_db_users_interval
                    << "<tr><td>write db files time<td class=ar>" << t - g_write_db_torrents_time << " / " << g_config.m_write_db_interval
                    << "<tr><td>write db users time<td class=ar>" << t - g_write_db_users_time << " / " << g_config.m_write_db_interval;
    }
    os << "</table>";
    return os;
}

t_user* find_user_by_torrent_pass(str_ref v, str_ref info_hash)
{
	if (v.size() != 32)
		return NULL;
	if (t_user* user = find_user_by_uid(read_int(4, hex_decode(v.substr(0, 8)))))
	{
		if (Csha1((boost::format("%s %d %d %s") % g_config.m_torrent_pass_private_key % user->torrent_pass_version % user->uid % info_hash).str()).read().substr(0, 12) == hex_decode(v.substr(8, 24)))
			return user;
	}
	return find_ptr2(g_users_torrent_passes, to_array<char, 32>(v));
}

void srv_term()
{
	g_sig_term = true;
}

void test_announce()
{
	t_user* u = find_ptr(g_users, 1);
	Ctracker_input i;
	i.m_info_hash = "IHIHIHIHIHIHIHIHIHIH";
	memcpy(i.m_peer_id.data(), str_ref("PIPIPIPIPIPIPIPIPIPI"));
	i.m_ipa = htonl(0x7f000063);
	i.m_port = 54321;
	cout << srv_insert_peer(i, false, u) << endl;
	write_db_torrents();
	write_db_users();
	g_time++;
	i.m_uploaded = 1 << 30;
	i.m_downloaded = 1 << 20;
	cout << srv_insert_peer(i, false, u) << endl;
	write_db_torrents();
	write_db_users();
	g_time += 3600;
	clean_up();
	write_db_torrents();
	write_db_users();
}
