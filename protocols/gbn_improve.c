 #include<stdio.h>
 #include<string.h>

 #include"protocol.h"
 #include"datalink.h"

 #define DATA_TIMER 	2000 
 #define ACK_TIMER      1100    
 #define MAX_SEQ 7
 #define inc(k) if (k<MAX_SEQ) k = k + 1; else k = 0;

 typedef unsigned char frame_kind;
 typedef unsigned char seq_nr;			//0-255
 typedef unsigned char packet[PKT_LEN];
 //typedef int event_type;


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
 	return ((a <= b && b < c) || (c < a&&b < c) || (c < a&&a <= b));
 }

 /*
 	补充校验和并传送给物理层。
 */
 static void put_frame(unsigned char *frame, int len)	// 课本的send_data
 {
 	*(unsigned int *)(frame + len) = crc32(frame, len);	// 添加32bit(4B)的校验位
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
 	s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);
 	stop_ack_timer();								// ack已捎带,停止计时。
 	// no-nak
 	if (fk == FRAME_DATA) {
 		s.seq = frame_nr;
 		memcpy(s.info, buffer[frame_nr], PKT_LEN);
 		dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.info);
 		put_frame((unsigned char *)&s, 3 + PKT_LEN);// 在info后插入4字节CRC
 		start_timer(frame_nr, DATA_TIMER);			// 开始计时
 	} else if (fk == FRAME_ACK) {
 		dbg_frame("Send ACK  %d\n", s.ack);
 		put_frame((unsigned char *)&s, 2);			// 在ack后插入4字节crc,覆盖seq和data
 	} else if (fk == FRAME_NAK) {
 		dbg_frame("Send NAK  %d\n", (s.ack + 1) % (MAX_SEQ + 1));
 		no_nak = 0;									// 出错帧NAK的序号为上一个成功帧的序号r.ack的下一帧
 		put_frame((unsigned char *)&s, 2);
 	}
 }

 int main(int argc, char**argv) {
 	seq_nr next_frame_to_send;	// 发送窗口上限
 	seq_nr ack_expected;		// 发送窗口下限
 	seq_nr frame_expected;		// 下一个准备接收的帧序号
 	frame r;
 	packet buffer[MAX_SEQ + 1];	// 发送窗口缓存
 	seq_nr nbuffered;

 	//seq_nr i;					// 需要吗
 	int event, arg;				// 事件, 以及timeout时的序号(用于debug)
 	int len = 0;				// 接收到的数据帧的长度


 	protocol_init(argc, argv);	// 协议初始化
	lprintf("No-for-version---ACK_TIMER=%dms, DATA_TIMER=%dms\n", ACK_TIMER, DATA_TIMER);	//打印超参数
 	lprintf("Designed by Deng Yangshen, build: " __DATE__"  "__TIME__"\n");

 	//disable_network_layer();	// 和书中相反,等待物理层准备好后再启用网络层
 	ack_expected = 0;			// Ws下限为0
 	next_frame_to_send = 0;		// Ws上限为0
 	frame_expected = 0;			// Wr为0
 	nbuffered = 0;				// 发送方缓存为空

 	while (1) {


 		if ((ack_expected + nbuffered) % (MAX_SEQ + 1) == next_frame_to_send) {//判断窗口大小是否正确
 			if (nbuffered < MAX_SEQ && phl_ready) {
 				enable_network_layer();
 			} else {
 				disable_network_layer();
 			}
 		} else {
 			dbg_event("%d-%d\n", (ack_expected + nbuffered) % (MAX_SEQ + 1), next_frame_to_send);
 			//重发数据,并等待接收事件
 			send_data(FRAME_DATA, next_frame_to_send, frame_expected, buffer);
 			inc(next_frame_to_send);
 			disable_network_layer();
 		}


 		event = wait_for_event(&arg);				//等待事件到达

 		switch (event) {
 		case NETWORK_LAYER_READY:
 			get_packet(buffer[next_frame_to_send]);	// 书上的是packet*类型,这里的是uchar类型,所以不需要&符
 			nbuffered++;
 			send_data(FRAME_DATA, next_frame_to_send, frame_expected, buffer);
 			inc(next_frame_to_send);				// 发送窗口上限向前滑动
 			break;

 		case PHYSICAL_LAYER_READY:
 			phl_ready = 1;							// 保存物理层状态
 			break;

 		case FRAME_RECEIVED:
 			len = recv_frame((unsigned char *)&r, sizeof(r));		//接收数据帧r

 			if (len < 6 || crc32((unsigned char*)&r, len) != 0) {	// 这里老师的代码是<5,我的代码是<6,因为最小是6个字节。
 				dbg_event("**** Receiver Error(length=%d), Bad CRC Checksum\n", len);
 				if (no_nak)
 					send_data(FRAME_NAK, 0, frame_expected, buffer);// 第2,4个参数没用,frame_expected就是出错的帧
 				break;												// 帧错误,丢弃
 			}

 			if (r.kind == FRAME_DATA) {
 				dbg_frame("Recv DATA %d %d, ID %d --%dbyte\n", r.seq, r.ack, *(short *)r.info, len);
 				if (r.seq == frame_expected) {		// 一位接收窗口
 					dbg_frame("Truee index.\n");
 					put_packet(r.info, len - 7);	// 将seq,ack,kind,crc32去掉后上交网络层
 					inc(frame_expected);			// 接收窗口滑动
 					no_nak = 1;						// 弹出一帧后
                    start_ack_timer(ACK_TIMER);			// 开始计时
 				} else if (no_nak) {					// 帧序号不对,发送nak
 					dbg_frame("Wrong seqnr %d received,send NAK.\n", r.seq);
 					send_data(FRAME_NAK, 0, frame_expected, buffer);
 				}
 			}


 			if (r.kind == FRAME_ACK)				// debug, can be delete
 				dbg_frame("Recv ACK  %d --%dbyte\n", r.ack, len);

 			if (r.kind == FRAME_NAK && (r.ack + 1) % (MAX_SEQ + 1) == ack_expected) {
 				dbg_frame("Recv NAK  %d --%dbyte\n", (r.ack + 1) % (MAX_SEQ + 1), len);
 				//如果返回的NAK就是我们期待的ACK,那就将整个窗口重发一遍,回退n帧。
 				//r.ack为下限的前一帧,不会触发下面的循环。
 				//照搬超时代码
 				next_frame_to_send = ack_expected;		// 从ack_expected开始

 				//暂时取消这个循环
 				//for (int i = 0; i < nbuffered; ++i) {	// 共nbuffered个数据	
 				//	send_data(FRAME_DATA, next_frame_to_send, frame_expected, buffer);
 				//	inc(next_frame_to_send);
 				//}
 			}

 			while (between(ack_expected, r.ack, next_frame_to_send)) {
 				nbuffered--;						// 缓冲区弹出对应元素
 				stop_timer(ack_expected);			// 下限停止计时
 				inc(ack_expected);					// 滑动下限
 			}
 			break;

 		case DATA_TIMEOUT:							// 重新发送buffer内的所有数据
 			dbg_event("---- DATA %d timeout\n", arg);
 			next_frame_to_send = ack_expected;		// 从ack_expected开始
 			//for (int i = 0; i < nbuffered; ++i) {	// 共nbuffered个数据	
 			//	send_data(FRAME_DATA, next_frame_to_send, frame_expected, buffer);
 			//	inc(next_frame_to_send);
 			//}
 			break;

 		case ACK_TIMEOUT:
 			send_data(FRAME_ACK, 0, frame_expected, buffer);//第二,四个参数对ack来说没有意义
 			break;
 		}

 	}
 }


