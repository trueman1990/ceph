
#include "include/types.h"
#include "include/Messenger.h"
#include "include/Message.h"

#include "mds/MDS.h"

#include "Client.h"
#include "ClNode.h"

#include "messages/MClientRequest.h"
#include "messages/MClientReply.h"

#include <stdlib.h>

#define debug 1

Client::Client(int id, Messenger *m)
{
  whoami = id;
  messenger = m;
  cwd = 0;
  root = 0;
  tid = 0;

  cache_lru.lru_set_max(5000);
  cache_lru.lru_set_midpoint(.5);

  max_requests = 1000;
}

Client::~Client()
{
  if (messenger) { delete messenger; messenger = 0; }
}

int Client::init()
{
  messenger->set_dispatcher(this);
  return 0;
}

int Client::shutdown()
{
  messenger->shutdown();
  return 0;
}



// dispatch

void Client::dispatch(Message *m) 
{
  switch (m->get_type()) {
  case MSG_CLIENT_REPLY:
	if (debug > 1)
	  cout << "client" << whoami << " got reply" << endl;
	assim_reply((MClientReply*)m);
	delete m;

	if (tid < max_requests)
	  issue_request();	
	break;

  default:
	cout << "client" << whoami << " got unknown message " << m->get_type() << endl;
  }
}
	

void Client::assim_reply(MClientReply *r)
{
  ClNode *cur = root;

  // add items to cache
  for (int i=0; i<r->trace.size(); i++) {
	if (i == 0) {
	  if (!root) {
		cur = root = new ClNode();
	  }
	} else {
	  if (cur->lookup(r->trace[i]->ref_dn) == NULL) {
		ClNode *n = new ClNode();
		cur->link( r->trace[i]->ref_dn, n );
		cur->isdir = true;  // clearly!
		cache_lru.lru_insert_top( n );
		cur = n;
	  } else {
		cur = cur->lookup( r->trace[i]->ref_dn );
		cache_lru.lru_touch(cur);
	  }
	}
	cur->ino = r->trace[i]->inode.ino;
	cur->dist = r->trace[i]->dist;
	cur->isdir = r->trace[i]->inode.isdir;
  }


  // add dir contents
  if (r->op == MDS_OP_READDIR) {
	cur->havedircontents = true;

	vector<c_inode_info*>::iterator it;
	for (it = r->dir_contents.begin(); it != r->dir_contents.end(); it++) {
	  if (cur->lookup((*it)->ref_dn)) 
		continue;  // skip if we already have it

	  ClNode *n = new ClNode();
	  n->ino = (*it)->inode.ino;
	  n->isdir = (*it)->inode.isdir;
	  n->dist = (*it)->dist;

	  cur->link( (*it)->ref_dn, n );
	  cache_lru.lru_insert_mid( n );

	  if (debug > 3)
		cout << "client got dir item " << (*it)->ref_dn << endl;
	}
  }

  cwd = cur;

  trim_cache();
}


void Client::trim_cache()
{
  int expired = 0;
  while (cache_lru.lru_get_size() > cache_lru.lru_get_max()) {
	ClNode *i = (ClNode*)cache_lru.lru_expire();
	if (!i) 
	  break;  // out of things to expire!
	
	i->detach();
	delete i;
	expired++;
  }
  if (debug > 1)
	cache_lru.lru_status();
  if (expired && debug > 2) 
	cout << "EXPIRED " << expired << " items" << endl;
}


void Client::issue_request()
{
  int op = MDS_OP_STAT;

  if (!cwd) cwd = root;
  string p = "";
  if (cwd) {
	
	// back out to a dir
	while (!cwd->isdir) 
	  cwd = cwd->parent;
	
	if (rand() % 10 > cwd->depth()) {
	  // descend
	  if (cwd->havedircontents) {
		if (debug > 3)
		  cout << "descending" << endl;
		// descend
		int n = rand() % cwd->children.size();
		hash_map<string, ClNode*>::iterator it = cwd->children.begin();
		while (n-- > 0) it++;
		cwd = (*it).second;
	  } else {
		// readdir
		if (debug > 3) cout << "readdir" << endl;
		op = MDS_OP_READDIR;
	  }
	} else {
	  // ascend
	  if (debug > 3) cout << "ascending" << endl;
	  if (cwd->parent)
		cwd = cwd->parent;
	}

	cwd->full_path(p);
  } 

  send_request(p, op);  // root, if !cwd
}

void Client::send_request(string& p, int op) 
{

  MClientRequest *req = new MClientRequest(tid++, op, whoami);
  req->ino = 1;
  req->path = p;

  // direct it
  int mds = 0;

  if (root) {
	int off = 0;
	ClNode *cur = root;
	while (off < req->path.length()) {
	  int nextslash = req->path.find('/', off);
	  if (nextslash == off) {
		off++;
		continue;
	  }
	  if (nextslash < 0) 
		nextslash = req->path.length();  // no more slashes
	  
	  string dname = req->path.substr(off,nextslash-off);
	  //cout << "//path segment is " << dname << endl;
	  
	  ClNode *n = cur->lookup(dname);
	  if (n) {
		cur = n;
		off = nextslash+1;
	  } else {
		if (debug > 3) cout << " don't have it. " << endl;
		int b = cur->dist.size();
		//cout << " b is " << b << endl;
		for (int i=0; i<b; i++) {
		  if (cur->dist[i]) {
			mds = i;
			break;
		  }
		}
		break;
	  }
	}
  } else {
	// we need the root inode
	mds = 0;
  }

  if (debug > 0)
	cout << "client" << whoami << " req " << tid << " op " << req->op << " to mds" << mds << " for " << req->path << endl;
  messenger->send_message(req,
						  MSG_ADDR_MDS(mds), MDS_PORT_SERVER,
						  0);
}
