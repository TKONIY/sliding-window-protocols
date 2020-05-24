#include<stdio.h>
#include<string.h>

#include"protocol.h"
#include"datalink.h"


#define DATA_TIMER 	6000		// 22*263
#define ACK_TIMER	5100		// 22*63-800
#define MAX_SEQ 43				// 序号的范围
#define NR_BUFS ((MAX_SEQ+1)/2)	// 缓冲区大小
#define inc(k) if (k<MAX_SEQ) k = k + 1; else k = 0;

typedef unsigned char frame_kind;
typedef unsigned char seq_nr;	//0-255
typedef unsigned char packet[PKT_LEN];
typedef unsigned char boolean;


typedef struct {
	frame_kind kind;	// 1 byte
	seq_nr ack;			// 1 byte
	seq_nr seq;			// 1 byte
	packet info;		// PKT_LEN byte -- 256
	unsigned int crc32;	// 4 byte -- crc32
	// 当帧类型为ack时,crc32放在info和seq的位置,接收的时候可以直接识别出长度。
}frame;

int phl_ready = 0;		// 初始化时默认物理层未准备好
static unsigned char no_nak = 1;		// 初始化,还没有发送过NAK

/*
	区间下(a)闭上(c)开, 用一个字节存返回值0/1
*/
static unsigned char between(seq_nr a, seq_nr b, seq_nr c) {
	return ((a <= b && b < c) || (c < a&& b < c) || (c < a&& a <= b));
}

/*
	补充校验和并传送给物理层。
*/
static void put_frame(unsigned char* frame, int len)	// 课本的send_data
{
	*(unsigned int*)(frame + len) = crc32(frame, len);	// 添加32bit(4B)的校验位
	send_frame(frame, len + 4);
	phl_ready = 0;										// 每次发完,先假设物理层缓存已满。
}

/*
	通过frame_expected倒推计算最近收到的帧的序号,作为ack返回。
	从buffer队列中取出packet发送。
	frame_nr为帧序号。
*/
static void send_data(frame_kind fk, seq_nr frame_nr, seq_nr frame_expected, packet buffer[]) {
	frame s;
	s.kind = fk;
	s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);	// 捎带ACK
	stop_ack_timer();									// ack已捎带,停止计时。

	if (fk == FRAME_DATA) {
		s.seq = frame_nr;
		memcpy(s.info, buffer[frame_nr % NR_BUFS], PKT_LEN);	// 将out_buf中对应该nr的内存复拷贝出来
		dbg_frame("Send DATA seq:%d ack:%d, ID %d\n", s.seq, s.ack, *(short*)s.info);
		put_frame((unsigned char*)&s, 3 + PKT_LEN);	// 在info后插入4字节CRC
		start_timer(frame_nr, DATA_TIMER);			// 开始计时,等待ACK
	} else if (fk == FRAME_ACK) {
		dbg_frame("Send ACK  %d\n", s.ack);
		put_frame((unsigned char*)&s, 2);			// 在ack后插入4字节crc,覆盖seq和data
	} else if (fk == FRAME_NAK) {
		dbg_frame("Send NAK  %d\n", (s.ack + 1) % (MAX_SEQ + 1));
		no_nak = 0;									// 出错帧NAK的序号为上一个成功帧的序号r.ack的下一帧
		put_frame((unsigned char*)&s, 2);			// 在ack后插入4字节crc,覆盖seq和data
	}
}

int main(int argc, char** argv) {
	seq_nr next_frame_to_send;	// 发送窗口上限+1
	seq_nr ack_expected;		// 发送窗口下限
	seq_nr frame_expected;		// 下一个准备接收的帧序号,接收窗口下限
	seq_nr too_far;				// 接收窗口上限+1
	frame r;					// 接收的数据帧
	packet in_buf[NR_BUFS];		// 接收窗口缓存
	packet out_buf[NR_BUFS];	// 发送窗口缓存
	boolean arrived[NR_BUFS];	// 表示in_buf的占用情况
	seq_nr nbuffered;			// 发送缓冲(已发送但未确认)的个数

	int event, arg;				// 事件, 以及timeout时的序号(用于debug)
	int len = 0;				// 接收到的数据帧的长度


	protocol_init(argc, argv);	// 协议初始化
	lprintf("Selective repeat---ACK_TIMER=%dms DATA_TIMER=%dms Windows=%d\n", ACK_TIMER, DATA_TIMER, NR_BUFS);
	lprintf("Designed by Deng Yangshen, build: " __DATE__"  "__TIME__"\n");

	disable_network_layer();	// 和书中相反,等待物理层准备好后再启用网络层
	ack_expected = 0;			// Ws下限为0
	next_frame_to_send = 0;		// Ws上限为0
	frame_expected = 0;			// Wr下限为0
	too_far = NR_BUFS;			// Wr上限为NR_BUFS
	nbuffered = 0;				// 发送缓冲初始化为0
	for (int i = 0; i < NR_BUFS; ++i)
		arrived[i] = 0;			// 接收缓冲全部初始化为空

	while (1) {

		event = wait_for_event(&arg);				//等待事件到达,如果datatimeout,则arg为超时的帧序号

		switch (event) {
		case NETWORK_LAYER_READY:
			nbuffered++;							// 已发送帧增加
			get_packet(out_buf[next_frame_to_send % NR_BUFS]);// 获取待发送帧
			send_data(FRAME_DATA, next_frame_to_send, frame_expected, out_buf);// 发送数据帧
			inc(next_frame_to_send);				// 发送窗口上限向前滑动
			break;

		case PHYSICAL_LAYER_READY:
			phl_ready = 1;	// 保存物理层状态
			break;

		case FRAME_RECEIVED:
			len = recv_frame((unsigned char*)&r, sizeof(r));			//接收数据帧r

			// 查错
			if (len < 6 || crc32((unsigned char*)&r, len) != 0) {		// 这里老师的代码是<5,我的代码是<6,因为最小是6个字节。
				dbg_event("**** Receiver Error(length=%d), Bad CRC Checksum\n", len);
				if (no_nak) {											// 如果已经发送过了nak,
					send_data(FRAME_NAK, 0, frame_expected, out_buf);	// 第2,4个参数无用,frame_expected就是出错的帧,发送一个NAK帧
				}
				break;													// 帧错误,丢弃
			}

			if (r.kind == FRAME_DATA) {
				dbg_frame("Recv DATA seq:%d ack:%d, ID %d |frame_e: %d  too_far:%d --%dbyte\n", r.seq, r.ack, *(short*)r.info, frame_expected, too_far, len);

				// 判断接收到的帧是不是接收窗口下界frame_expected
				if ((r.seq != frame_expected) && no_nak) {
					dbg_event(" Recv frame is not lower bound, NAK sent back\n");
					send_data(FRAME_NAK, 0, frame_expected, out_buf);	// 发送NAK, 第2,4个参数无用
				}

				// 判断接收到的帧是否在接收窗口上下界之间,并判断接收buffer中还有没有属于它的位置
				if (between(frame_expected, r.seq, too_far) && (arrived[r.seq % NR_BUFS] == 0)) {
					arrived[r.seq % NR_BUFS] = 1;						// 将接收缓冲的对应位置占下
					memcpy(in_buf[r.seq % NR_BUFS], r.info, PKT_LEN);	// 将该包加入接收缓冲中
					while (arrived[frame_expected % NR_BUFS]) {			// 滑动更新接收窗口和接收缓冲区
						dbg_event("Put packet to network layer seq:%d, ID: %d\n", frame_expected, *(short*)(in_buf[frame_expected % NR_BUFS]));
						put_packet(in_buf[frame_expected % NR_BUFS], PKT_LEN);	// 将缓冲区最早到达的帧上交网络层
						arrived[frame_expected % NR_BUFS] = 0;			// 接收缓冲区空出这个位置
						no_nak = 1;										// 新的一帧没有发送过nak
						inc(frame_expected);							// 滑动下界
						inc(too_far);									// 滑动上界
						start_ack_timer(ACK_TIMER);						// 开始ACK计时,准备返回ACK
					}
				}
			}


			if (r.kind == FRAME_NAK && between(ack_expected, (r.ack + 1) % (MAX_SEQ + 1), next_frame_to_send)) {
				dbg_frame("Recv NAK  %d --%dbyte\n", (r.ack + 1) % (MAX_SEQ + 1), len);
				//如果返回的NAK就是ack_expected,那就将这一帧重发一遍
				//r.ack表示接收方成功接收的最后一帧,(r.ack + 1) % (MAX_SEQ + 1)就是接收失败的那一帧
				send_data(FRAME_DATA, (r.ack + 1) % (MAX_SEQ + 1), frame_expected, out_buf);
			}

			if (r.kind == FRAME_ACK)				// debug, 可以删除, 对于ack的处理在下面
				dbg_frame("Recv ACK  %d --%dbyte\n", r.ack, len);

			while (between(ack_expected, r.ack, next_frame_to_send)) {
				nbuffered--;						// 发送缓冲弹出确认接收的帧
				stop_timer(ack_expected);			// 下限停止计时
				inc(ack_expected);					// 发送窗口下限向前滑动
			}
			break;

		case DATA_TIMEOUT:							// 超时未收到ack
			dbg_event("---- DATA %d timeout,resend\n", arg);
			send_data(FRAME_DATA, arg, frame_expected, out_buf);//重发超时的那一帧
			break;

		case ACK_TIMEOUT:
			send_data(FRAME_ACK, 0, frame_expected, out_buf);	//第二,四个参数对ack来说没有意义
			break;
		}

		if (nbuffered < NR_BUFS && phl_ready)		// 发送窗口为NR_BUFS,缓冲满时暂停发送
			enable_network_layer();
		else
			disable_network_layer();

	}
}


