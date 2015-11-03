// #define DEBUG // Allow debugging
// #define DEBUG2 // Allow debugging

#include <os>
#include <net/class_arp.hpp>

#include <vector>

using namespace net;

int Arp::bottom(std::shared_ptr<Packet>& pckt)
{
  debug2("<ARP handler> got %li bytes of data \n", pckt->len());

  header* hdr = (header*) pckt->buffer();
  //debug2("\t OPCODE: 0x%x \n",hdr->opcode);
  //debug2("Chaching IP %s for %s \n", hdr->sipaddr.str().c_str() , hdr->shwaddr.str().c_str())
  debug2("Have valid cache? %s \n",is_valid_cached(hdr->sipaddr) ? "YES":"NO");
  cache(hdr->sipaddr, hdr->shwaddr);
  
  switch(hdr->opcode){
    
  case ARP_OP_REQUEST:
    debug2("\t ARP REQUEST: ");
    debug2("%s is looking for %s \n",
          hdr->sipaddr.str().c_str(),hdr->dipaddr.str().c_str());
    
    if (hdr->dipaddr == _ip)
      arp_respond(hdr);    
    else{ debug2("\t NO MATCH for My IP. DROP!\n"); }
        
    break;
    
  case ARP_OP_REPLY:    
    debug2("\t ARP REPLY: %s belongs to %s\n",
          hdr->sipaddr.str().c_str(), hdr->shwaddr.str().c_str())
    break;
    
  default:
    debug2("\t UNKNOWN OPCODE \n");
    break;
  }
  
  // Free the buffer (We're leaf node for this one's path)
  // @todo Freeing here corrupts the outgoing frame. Why?
  //free(data);
  
  return 0 + 0 * pckt->len(); // yep, it's what you think it is (and what's that?!)
};
  

void Arp::cache(IP4::addr& ip, Ethernet::addr& mac){
  
  debug2("Chaching IP %s for %s \n",ip.str().c_str(),mac.str().c_str());
  
  auto entry = _cache.find(ip);
  if (entry != _cache.end()){
    
    debug2("Cached entry found: %s recorded @ %li. Updating timestamp \n",
          entry->second._mac.str().c_str(), entry->second._t);
    
    // Update
    entry->second.update();
    
  }else _cache[ip] = mac; // Insert
  
}


bool Arp::is_valid_cached(IP4::addr& ip){
  auto entry = _cache.find(ip);
  return entry != _cache.end() 
    and (entry->second._t + cache_exp_t > OS::uptime());
}

extern "C" {
  unsigned long ether_crc(int length, unsigned char *data);
}

int Arp::arp_respond(header* hdr_in){
  debug2("\t IP Match. Constructing ARP Reply \n");
  
  // Allocate send buffer
  int bufsize = sizeof(header);
  uint8_t* buffer = (uint8_t*)malloc(bufsize);
  header* hdr = (header*)buffer;
  
  // Populate ARP-header
  
  // Scalar 
  hdr->htype = hdr_in->htype;
  hdr->ptype = hdr_in->ptype;
  hdr->hlen_plen = hdr_in->hlen_plen;  
  hdr->opcode = ARP_OP_REPLY;
  
  hdr->dipaddr.whole = hdr_in->sipaddr.whole;
  hdr->sipaddr.whole = _ip.whole;
  
  // Composite  
  hdr->shwaddr.minor = _mac.minor;
  hdr->shwaddr.major = _mac.major;
  
  hdr->dhwaddr.minor = hdr_in->shwaddr.minor;
  hdr->dhwaddr.major = hdr_in->shwaddr.major;
  
  debug2("\t My IP: %s belongs to My Mac: %s \n ",
        hdr->sipaddr.str().c_str(), hdr->shwaddr.str().c_str());
   
  // (partially) Populate Ethernet header
  hdr->ethhdr.dest.minor = hdr->dhwaddr.minor;
  hdr->ethhdr.dest.major = hdr->dhwaddr.major;
  hdr->ethhdr.type = Ethernet::ETH_ARP;    
  
  // We're passing a stack-pointer here. That's dangerous if the packet 
  // is supposed to be kept, somewhere up the stack. 
  auto packet_ptr = std::make_shared<Packet>
    (Packet(buffer, bufsize, Packet::DOWNSTREAM));
  
  _linklayer_out(packet_ptr);
  
  return 0;
}


static int ignore(std::shared_ptr<Packet> UNUSED(pckt)){
  debug2("<ARP -> linklayer> Empty handler - DROP!\n");
  return -1;
}


int Arp::transmit(std::shared_ptr<Packet>& pckt){
  
  /** Get destination IP from IP header   */
  IP4::ip_header* iphdr = (IP4::ip_header*)(pckt->buffer() 
                                            + sizeof(Ethernet::header));
  IP4::addr sip = iphdr->saddr;
  IP4::addr dip = pckt->next_hop();

  debug2("<ARP -> physical> Transmitting %li bytes to %s \n",
        pckt->len(),dip.str().c_str());
  
  if (sip != _ip) {
    debug2("<ARP -> physical> Not bound to source IP %s. My IP is %s. DROP!\n",
          sip.str().c_str(), _ip.str().c_str());            
    return -1;
  }
  
  Ethernet::addr mac;

  // If we don't have a cached IP, get mac from next-hop (Håreks c001 hack)
  if (!is_valid_cached(dip)){

    // Fixed mac prefix
    mac.minor = 0x01c0; //Big-endian c001
    // Destination IP
    mac.major = dip.whole;
    debug("ARP cache missing. Guessing Mac %s from next-hop IP: %s (dest.ip: %s)",
          mac.str().c_str(), dip.str().c_str(), iphdr->daddr.str().c_str());
    
  }else{
    // Get mac from cache
    mac = _cache[dip]._mac;
  }
  
  /** Attach next-hop mac and ethertype to ethernet header  */  
  Ethernet::header* ethhdr = (Ethernet::header*)pckt->buffer();    
  ethhdr->dest.major = mac.major;
  ethhdr->dest.minor = mac.minor;
  ethhdr->type = Ethernet::ETH_IP4;
  
  debug2("<ARP -> physical> Sending packet to %s \n",mac.str().c_str());

  return _linklayer_out(pckt);
  
  return 0;
};

// Initialize
Arp::Arp(Ethernet::addr mac,IP4::addr ip): 
  _mac(mac), _ip(ip),
  _linklayer_out(downstream(ignore))
{}