#include "core/ext4_writer.h"
#include <cstring>
#include <algorithm>

namespace hw::core {
namespace {

uint16_t rd16(const uint8_t* p){ return (uint16_t)(p[0] | (p[1]<<8)); }
uint32_t rd32(const uint8_t* p){ return p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }
void wr16(uint8_t* p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
void wr32(uint8_t* p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

uint16_t crc16(uint16_t crc, const uint8_t* p, size_t len){
    while(len--){ crc ^= *p++; for(int i=0;i<8;i++) crc = (crc&1) ? (crc>>1)^0xA001 : crc>>1; }
    return crc;
}

struct Fs {
    uint8_t* img; size_t img_len;
    uint32_t block_size=0, blocks_per_group=0, inodes_per_group=0, inode_size=0, first_ino=0;
    uint32_t inodes_count=0, blocks_count=0, first_data_block=0, groups=0, desc_size=32;
    uint32_t feat_ro=0, feat_incompat=0; uint8_t uuid[16]; size_t gdt_off=0;

    uint8_t* sb(){ return img+1024; }
    uint8_t* blk(uint32_t b){ return img + (size_t)b*block_size; }
    uint8_t* gd(uint32_t g){ return img+gdt_off + (size_t)g*desc_size; }

    bool init(std::string& err){
        uint8_t* s=sb();
        if(rd16(s+0x38)!=0xEF53){ err="bad ext4 magic"; return false; }
        inodes_count=rd32(s+0x00); blocks_count=rd32(s+0x04); first_data_block=rd32(s+0x14);
        block_size = 1024u << rd32(s+0x18);
        blocks_per_group=rd32(s+0x20); inodes_per_group=rd32(s+0x28);
        first_ino=rd32(s+0x54); inode_size=rd16(s+0x58);
        feat_incompat=rd32(s+0x60); feat_ro=rd32(s+0x64);
        std::memcpy(uuid, s+0x68, 16);
        if(feat_incompat & 0x80){ desc_size=rd16(s+0xFE); if(desc_size<32)desc_size=32; }
        if(!block_size || !blocks_per_group || !inodes_per_group){ err="bad ext4 geometry"; return false; }
        groups = (blocks_count - first_data_block + blocks_per_group - 1)/blocks_per_group;
        gdt_off = (size_t)(first_data_block+1)*block_size;
        return true;
    }
    void gd_csum(uint32_t g){
        if(!(feat_ro & 0x10)) return;
        uint8_t* d=gd(g); wr16(d+0x1E,0);
        uint8_t lg[4]={(uint8_t)g,(uint8_t)(g>>8),(uint8_t)(g>>16),(uint8_t)(g>>24)};
        uint16_t c=crc16(0xFFFF, uuid, 16); c=crc16(c,lg,4); c=crc16(c,d,0x1E);
        if(desc_size>32) c=crc16(c,d+0x20,desc_size-32);
        wr16(d+0x1E,c);
    }
    bool alloc_block(uint32_t& out){
        for(uint32_t g=0; g<groups; g++){
            uint8_t* d=gd(g); uint8_t* bm=blk(rd32(d+0x00));
            uint32_t base=first_data_block+g*blocks_per_group;
            uint32_t cnt=(g==groups-1)?(blocks_count-base):blocks_per_group;
            for(uint32_t i=0;i<cnt;i++) if(!(bm[i>>3]&(1<<(i&7)))){
                bm[i>>3]|=(1<<(i&7));
                uint16_t fb=rd16(d+0x0C); if(fb) wr16(d+0x0C,fb-1);
                uint16_t fl=rd16(d+0x12); fl&=~0x2; wr16(d+0x12,fl);
                gd_csum(g);
                uint8_t* s=sb(); uint32_t sfb=rd32(s+0x0C); if(sfb) wr32(s+0x0C,sfb-1);
                out=base+i; return true;
            }
        }
        return false;
    }
    bool alloc_inode(uint32_t& out){
        for(uint32_t g=0; g<groups; g++){
            uint8_t* d=gd(g); uint8_t* bm=blk(rd32(d+0x04));
            for(uint32_t i=0;i<inodes_per_group;i++){
                uint32_t ino=g*inodes_per_group+i+1;
                if(ino<first_ino) continue;
                if(!(bm[i>>3]&(1<<(i&7)))){
                    bm[i>>3]|=(1<<(i&7));
                    uint16_t fi=rd16(d+0x0E); if(fi) wr16(d+0x0E,fi-1);
                    uint16_t fl=rd16(d+0x12); fl&=~0x1; wr16(d+0x12,fl);
                    uint16_t unused=rd16(d+0x1C); uint32_t used_hi=inodes_per_group-unused;
                    if(i+1>used_hi) wr16(d+0x1C,(uint16_t)(inodes_per_group-(i+1)));
                    gd_csum(g);
                    uint8_t* s=sb(); uint32_t sfi=rd32(s+0x10); if(sfi) wr32(s+0x10,sfi-1);
                    out=ino; return true;
                }
            }
        }
        return false;
    }
    uint8_t* inode_ptr(uint32_t ino){
        uint32_t g=(ino-1)/inodes_per_group, idx=(ino-1)%inodes_per_group;
        return blk(rd32(gd(g)+0x08)) + (size_t)idx*inode_size;
    }
    bool dir_first_block(uint32_t ino, uint32_t& out, std::string& err){
        uint8_t* n=inode_ptr(ino); uint32_t flags=rd32(n+0x20); uint8_t* ib=n+0x28;
        if(flags&0x80000){
            if(rd16(ib+0)!=0xF30A){ err="dir not extent-magic"; return false; }
            if(rd16(ib+6)!=0 || rd16(ib+2)<1){ err="dir extent depth/entries unsupported"; return false; }
            out=rd32(ib+12+8); return true;
        }
        out=rd32(ib+0); return true;
    }
    // All data blocks of a directory inode (extent depth 0 or classic direct map).
    std::vector<uint32_t> dir_blocks(uint32_t ino){
        std::vector<uint32_t> blks;
        uint8_t* n=inode_ptr(ino); uint32_t flags=rd32(n+0x20); uint8_t* ib=n+0x28;
        if(flags&0x80000){
            if(rd16(ib+0)!=0xF30A || rd16(ib+6)!=0) return blks;      // need extent magic, depth 0
            uint16_t entries=rd16(ib+2);
            for(uint16_t i=0;i<entries && i<4;i++){ uint8_t* ex=ib+12+(size_t)i*12;
                uint16_t len=rd16(ex+4); uint32_t start=rd32(ex+8);
                for(uint16_t j=0;j<len;j++) blks.push_back(start+j); }
        } else {
            for(int i=0;i<12;i++){ uint32_t b=rd32(ib+(size_t)i*4); if(b) blks.push_back(b); }
        }
        return blks;
    }
    // Find `name` in directory `parent`, scanning every data block (HTree leaf
    // blocks carry real entries, so a linear scan resolves indexed dirs too).
    uint32_t lookup(uint32_t parent, const std::string& name){
        for(uint32_t b : dir_blocks(parent)){
            uint8_t* dir=blk(b); uint32_t off=0;
            while(off+8<=block_size){
                uint8_t* e=dir+off; uint32_t e_ino=rd32(e); uint16_t rec=rd16(e+4); uint8_t nl=e[6];
                if(rec<8) break;
                if(e_ino!=0 && nl==name.size() && std::memcmp(e+8,name.data(),nl)==0) return e_ino;
                off+=rec;
            }
        }
        return 0;
    }
    // Resolve a '/'-separated path's PARENT directory inode + leaf name.
    bool resolve_parent(const std::string& path, uint32_t& parent, std::string& leaf, std::string& err){
        std::vector<std::string> parts; size_t i=0;
        while(i<path.size()){ if(path[i]=='/'){ ++i; continue; } size_t j=path.find('/',i);
            if(j==std::string::npos) j=path.size(); parts.push_back(path.substr(i,j-i)); i=j; }
        if(parts.empty()){ err="empty path"; return false; }
        leaf=parts.back();
        uint32_t cur=2;                                              // root inode
        for(size_t k=0;k+1<parts.size();k++){
            uint32_t nxt=lookup(cur,parts[k]);
            if(!nxt){ err="path component not found: "+parts[k]; return false; }
            cur=nxt;
        }
        parent=cur; return true;
    }
};

} // namespace

bool ext4_add_file(Bytes& img, const std::string& path,
                   const std::vector<uint8_t>& content, uint32_t mode, std::string& err){
    Fs fs; fs.img=img.data(); fs.img_len=img.size();
    if(!fs.init(err)) return false;
    uint32_t parent_ino; std::string name;
    if(!fs.resolve_parent(path, parent_ino, name, err)) return false;

    size_t clen=content.size();
    uint32_t need=(uint32_t)((clen+fs.block_size-1)/fs.block_size);
    std::vector<uint32_t> dblocks;
    for(uint32_t i=0;i<need;i++){ uint32_t b; if(!fs.alloc_block(b)){ err="no free block"; return false; } dblocks.push_back(b); }
    for(uint32_t i=1;i<need;i++) if(dblocks[i]!=dblocks[0]+i){ err="non-contiguous allocation (unsupported)"; return false; }
    for(uint32_t i=0;i<need;i++){
        uint8_t* b=fs.blk(dblocks[i]); size_t off=(size_t)i*fs.block_size;
        size_t n=(off<clen)?std::min((size_t)fs.block_size,clen-off):0;
        std::memset(b,0,fs.block_size); if(n) std::memcpy(b,content.data()+off,n);
    }
    uint32_t ino; if(!fs.alloc_inode(ino)){ err="no free inode"; return false; }

    uint8_t* n=fs.inode_ptr(ino); std::memset(n,0,fs.inode_size);
    wr16(n+0x00,(uint16_t)(0x8000|(mode&0xFFF)));
    wr32(n+0x04,(uint32_t)clen);
    uint32_t now=1735689600u; wr32(n+0x08,now); wr32(n+0x0C,now); wr32(n+0x10,now);
    wr16(n+0x1A,1); wr32(n+0x1C,need*(fs.block_size/512)); wr32(n+0x20,0x80000);
    uint8_t* ib=n+0x28;
    wr16(ib+0,0xF30A); wr16(ib+2,(uint16_t)(need?1:0)); wr16(ib+4,4); wr16(ib+6,0); wr32(ib+8,0);
    if(need){ uint8_t* ex=ib+12; wr32(ex+0,0); wr16(ex+4,(uint16_t)need); wr16(ex+6,0); wr32(ex+8,dblocks[0]); }
    if(fs.inode_size>128) wr16(n+0x80,32);

    uint32_t pblk; if(!fs.dir_first_block(parent_ino,pblk,err)) return false;
    uint8_t* dir=fs.blk(pblk);
    uint16_t need_rec=(uint16_t)((8+name.size()+3)&~3);
    uint32_t off=0; bool placed=false;
    while(off<fs.block_size){
        uint8_t* e=dir+off; uint32_t e_ino=rd32(e+0); uint16_t rec=rd16(e+4); uint8_t nl=e[6];
        if(rec==0) break;
        uint16_t used=(uint16_t)((8+nl+3)&~3);
        uint16_t slack=(e_ino==0)?rec:(uint16_t)(rec-used);
        if(slack>=need_rec){
            uint16_t new_off; uint16_t new_rec;
            if(e_ino==0){ new_off=(uint16_t)off; new_rec=rec; }
            else { wr16(e+4,used); new_off=(uint16_t)(off+used); new_rec=(uint16_t)(rec-used); }
            uint8_t* ne=dir+new_off;
            wr32(ne+0,ino); wr16(ne+4,new_rec); ne[6]=(uint8_t)name.size(); ne[7]=1;
            std::memcpy(ne+8,name.data(),name.size());
            placed=true; break;
        }
        off+=rec;
    }
    if(!placed){ err="no room in parent dir block (HTree/expand unsupported)"; return false; }
    return true;
}

bool ext4_add_root_file(Bytes& img, const std::string& name,
                        const std::vector<uint8_t>& content, uint32_t mode, std::string& err){
    if(name.empty() || name.find('/')!=std::string::npos){ err="name must be a single root-level component"; return false; }
    return ext4_add_file(img, "/"+name, content, mode, err);
}

bool ext4_overwrite_file(Bytes& img, const std::string& path,
                         const std::vector<uint8_t>& content, std::string& err){
    Fs fs; fs.img=img.data(); fs.img_len=img.size();
    if(!fs.init(err)) return false;
    uint32_t parent_ino; std::string name;
    if(!fs.resolve_parent(path, parent_ino, name, err)) return false;
    uint32_t ino=fs.lookup(parent_ino, name);
    if(!ino){ err="file not found: "+path; return false; }
    // Existing data blocks (extent depth-0 or direct map), must be contiguous so
    // the new bytes can be written straight over them.
    std::vector<uint32_t> blocks=fs.dir_blocks(ino);
    if(blocks.empty()){ err="file has no data blocks / unsupported layout"; return false; }
    for(size_t i=1;i<blocks.size();i++)
        if(blocks[i]!=blocks[0]+i){ err="file not contiguous (unsupported)"; return false; }
    size_t clen=content.size();
    uint32_t need=(uint32_t)((clen+fs.block_size-1)/fs.block_size);
    if(need>blocks.size()){
        err="new content ("+std::to_string(clen)+" B / "+std::to_string(need)+
            " blocks) exceeds allocation ("+std::to_string(blocks.size())+" blocks)"; return false;
    }
    for(uint32_t i=0;i<need;i++){
        uint8_t* b=fs.blk(blocks[0]+i); size_t off=(size_t)i*fs.block_size;
        size_t n=(off<clen)?std::min((size_t)fs.block_size,clen-off):0;
        std::memset(b,0,fs.block_size); if(n) std::memcpy(b,content.data()+off,n);
    }
    // Update the size (low 32b; high 32b at 0x6C stays 0 for our <4GB files). Block
    // allocation is unchanged, so i_blocks is left as-is.
    uint8_t* n=fs.inode_ptr(ino);
    wr32(n+0x04,(uint32_t)clen);
    wr32(n+0x6C,0);
    return true;
}

} // namespace hw::core
