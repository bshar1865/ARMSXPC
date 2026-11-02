#include "stdafx.h"
#include "xmb_p3t_animation.h"

#include <QFile>
#include <QHash>
#include <QPainter>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <zlib.h>

namespace
{
	quint32 be32(const QByteArray& b, qsizetype at) { return at >= 0 && at + 4 <= b.size() ? qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(b.constData() + at)) : 0xffffffffu; }
	quint32 le32(const uchar* p) { return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24); }
	quint16 le16(const uchar* p) { return quint16(p[0]) | (quint16(p[1]) << 8); }
	QString str(const QByteArray& b, quint32 at) { if (at >= quint32(b.size())) return {}; qsizetype end = at; while (end < b.size() && b[end]) ++end; return QString::fromLatin1(b.constData() + at, end - at); }
	QByteArray inflate_raf(const QByteArray& packed)
	{
		if (packed.size() < 8 || packed.left(4) != QByteArrayLiteral("_RAF")) return {};
		z_stream zs{}; zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(packed.constData() + 8)); zs.avail_in = static_cast<uInt>(packed.size() - 8);
		if (inflateInit(&zs) != Z_OK) return {};
		QByteArray out; std::array<char, 64 * 1024> chunk{}; int rc = Z_OK;
		while (rc == Z_OK)
		{
			zs.next_out = reinterpret_cast<Bytef*>(chunk.data()); zs.avail_out = static_cast<uInt>(chunk.size()); rc = inflate(&zs, Z_NO_FLUSH);
			const qsizetype made = chunk.size() - zs.avail_out;
			if (out.size() + made > (256ll << 20)) { rc = Z_MEM_ERROR; break; }
			out.append(chunk.data(), made);
		}
		inflateEnd(&zs); return rc == Z_STREAM_END ? out : QByteArray{};
	}
	int expand5(int x) { return (x << 3) | (x >> 2); }
	int expand6(int x) { return (x << 2) | (x >> 4); }
	QImage decode_gtf(const QByteArray& b)
	{
		if (b.size() < 48 || be32(b, 8) < 1) return {};
		const quint32 data = be32(b, 16), length = be32(b, 20), fmt = uchar(b[24]) & ~0x60u;
		const int width = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(b.constData() + 32));
		const int height = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(b.constData() + 34));
		if (width < 1 || height < 1 || width > 4096 || height > 4096 || (fmt != 0x86 && fmt != 0x87 && fmt != 0x88)) return {};
		const int block = fmt == 0x86 ? 8 : 16, row = std::max<int>(qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(b.constData() + 40)), ((width + 3) / 4) * block);
		const quint64 needed = quint64(row) * ((height + 3) / 4 - 1) + ((width + 3) / 4) * block;
		if (data > quint32(b.size()) || length > quint32(b.size()) - data || needed > length || needed > (16ull << 20)) return {};
		QImage image(width, height, QImage::Format_ARGB32); if (image.isNull()) return {};
		for (int by = 0; by < (height + 3) / 4; ++by) for (int bx = 0; bx < (width + 3) / 4; ++bx)
		{
			const uchar* p = reinterpret_cast<const uchar*>(b.constData() + data + by * row + bx * block);
			int alpha[16]; std::fill(std::begin(alpha), std::end(alpha), 255); const uchar* c = p;
			if (fmt == 0x87) { for (int i=0;i<16;++i) alpha[i] = ((p[i/2] >> ((i&1)*4)) & 15)*17; c += 8; }
			else if (fmt == 0x88)
			{
				const int a0=p[0], a1=p[1]; quint64 bits=0; for(int i=0;i<6;++i) bits |= quint64(p[2+i]) << (8*i);
				for(int i=0;i<16;++i) { const int k=(bits>>(3*i))&7; alpha[i]=k==0?a0:k==1?a1:(a0>a1?((8-k)*a0+(k-1)*a1)/7:(k==6?0:k==7?255:((6-k)*a0+(k-1)*a1)/5)); } c += 8;
			}
			const int c0=le16(c), c1=le16(c+2); const int r0=expand5(c0>>11),g0=expand6((c0>>5)&63),b0=expand5(c0&31),r1=expand5(c1>>11),g1=expand6((c1>>5)&63),b1=expand5(c1&31);
			QRgb colors[4]={qRgb(r0,g0,b0),qRgb(r1,g1,b1),0,0};
			if(fmt!=0x86 || c0>c1) { colors[2]=qRgb((2*r0+r1)/3,(2*g0+g1)/3,(2*b0+b1)/3); colors[3]=qRgb((r0+2*r1)/3,(g0+2*g1)/3,(b0+2*b1)/3); }
			else { colors[2]=qRgb((r0+r1)/2,(g0+g1)/2,(b0+b1)/2); colors[3]=qRgba(0,0,0,0); }
			const quint32 indices=le32(c+4);
			for(int i=0;i<16;++i) { const int x=bx*4+i%4,y=by*4+i/4;if(x>=width||y>=height)continue; const QRgb rgb=colors[(indices>>(2*i))&3]; image.setPixel(x,y,qRgba(qRed(rgb),qGreen(rgb),qBlue(rgb),fmt==0x86?qAlpha(rgb):alpha[i])); }
		}
		return image;
	}
}

xmb_p3t_animation::result xmb_p3t_animation::read(const QString& path, quint64 offset, quint64 size)
{
	result out; QFile f(path); if (!f.open(QIODevice::ReadOnly) || size < 8 || size > (128ull<<20) || offset > quint64(f.size()) || size > quint64(f.size())-offset || !f.seek(qint64(offset))) return out;
	const QByteArray packed=f.read(qint64(size)); const QByteArray raf=inflate_raf(packed); if(raf.size()<56 || be32(raf,0)!=0x5241464f) return out;
	struct span{quint32 off,size;}; span tabs[6]{}; for(int i=0;i<6;++i){tabs[i]={be32(raf,8+i*8),be32(raf,12+i*8)};const quint32 limit=i==5?(256u<<20):(16u<<20);if(tabs[i].off>quint32(raf.size())||tabs[i].size>quint32(raf.size())-tabs[i].off||tabs[i].size>limit)return out;}
	const QByteArray elems=raf.mid(tabs[0].off,tabs[0].size), ids=raf.mid(tabs[1].off,tabs[1].size), names=raf.mid(tabs[2].off,tabs[2].size), files=raf.mid(tabs[5].off,tabs[5].size);
	struct node{quint32 id=0,ref=0,parent=0,src=0,size=0;}; QHash<quint32,quint32> mat_ids,mat_tex,tex_file,file_offsets; QHash<quint32,quint32> file_sizes; struct actor{quint32 material;}; QVector<actor> actors;
	for(quint64 pos=0;pos+28<=quint64(elems.size());)
	{
		const qsizetype at=qsizetype(pos); const QString n=str(names,be32(elems,at)); const quint32 count=be32(elems,at+4),parent=be32(elems,at+8); if(count>(quint64(elems.size())-pos-28)/16) return out;
		quint32 id=0xffffffffu,ref=0xffffffffu,src=0,sizev=0;
		for(quint32 i=0;i<count;++i){const qsizetype a=at+28+qsizetype(i)*16; const QString key=str(names,be32(elems,a));const quint32 type=be32(elems,a+4),v=be32(elems,a+8),z=be32(elems,a+12);if(key=="id"&&(type==7||type==8))id=v;else if((key=="material"||key=="texref"||key=="fileref")&&(type==7||type==8))ref=v;else if(key=="src"&&type==6){src=v;sizev=z;}}
		if(n=="actor"&&ref!=0xffffffffu) actors.push_back({ref}); else if(n=="material"&&id!=0xffffffffu)mat_ids.insert(quint32(pos),id);else if(n=="_texture"&&ref!=0xffffffffu&&mat_ids.contains(parent))mat_tex[mat_ids.value(parent)]=ref;else if(n=="texture"&&id!=0xffffffffu&&ref!=0xffffffffu)tex_file[id]=ref;else if(n=="file"&&id!=0xffffffffu&&sizev){file_offsets[id]=src;file_sizes[id]=sizev;}
		pos+=28+quint64(count)*16;
	}
	QVector<QImage> slides, overlays;
	for(const actor& a:actors)
	{
		const quint32 file_id=tex_file.value(mat_tex.value(a.material),0xffffffffu); if(!file_offsets.contains(file_id))continue;
		const quint32 off=file_offsets.value(file_id), len=file_sizes.value(file_id);if(off>quint32(files.size())||len>quint32(files.size())-off)continue;
		const QByteArray texture = files.mid(off,len);
		QImage img=decode_gtf(texture); if(img.isNull()) img.loadFromData(texture);
		if(img.width()<960 || img.height()==0 || img.width()<1.5*img.height() || img.width()>2.0*img.height())continue;
		quint64 clear=0,total=quint64(img.width())*img.height(); for(int y=0;y<img.height();++y){const QRgb* line=reinterpret_cast<const QRgb*>(img.constScanLine(y));for(int x=0;x<img.width();++x)if(qAlpha(line[x])<250)++clear;}
		const bool is_dxt1 = texture.size() >= 48 && ((uchar(texture.at(24))&~0x60u)==0x86);
		const bool is_slide=is_dxt1 || clear*100<=total;
		if(is_slide){QImage frame(img.size(),QImage::Format_ARGB32);frame.fill(Qt::transparent);QPainter p(&frame);for(auto it=overlays.crbegin();it!=overlays.crend();++it)p.drawImage(0,0,*it);p.drawImage(0,0,img);p.end();slides.push_back(frame);overlays.clear();if(slides.size()>=24)break;}else overlays.prepend(img);
	}
	for(const QImage& frame:slides)if(!frame.isNull())out.frames.push_back(frame);
	return out;
}
