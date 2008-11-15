/*
 * Copyright (C) 2008  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#include "psimediaprovider.h"

#include <QtCore>
#include <QImage>
#include <gst/gst.h>
#include "gstcustomelements.h"
#include "devices.h"
#include "payloadinfo.h"

namespace PsiMedia {

static QList<GstDevice> gstAudioOutputDevices()
{
	return devices_list(PDevice::AudioOut);
}

static QList<GstDevice> gstAudioInputDevices()
{
	return devices_list(PDevice::AudioIn);
}

static QList<GstDevice> gstVideoInputDevices()
{
	return devices_list(PDevice::VideoIn);
}

static PDevice gstDeviceToPDevice(const GstDevice &dev, PDevice::Type type)
{
	PDevice out;
	out.type = type;
	out.name = dev.name;
	out.id = dev.id;
	return out;
}

static GstElement *make_device_element(const QString &id, PDevice::Type type, QSize *captureSize = 0)
{
	return devices_makeElement(id, type, captureSize);
}

//----------------------------------------------------------------------------
// GstSession
//----------------------------------------------------------------------------
// converts Qt-ified commandline args back to C style
class CArgs
{
public:
	int argc;
	char **argv;

	CArgs()
	{
		argc = 0;
		argv = 0;
	}

	~CArgs()
	{
		if(count > 0)
		{
			for(int n = 0; n < count; ++n)
				free(data[n]);
			free(argv);
			free(data);
		}
	}

	void set(const QStringList &args)
	{
		count = args.count();
		if(count == 0)
		{
			data = 0;
			argc = 0;
			argv = 0;
		}
		else
		{
			data = (char **)malloc(sizeof(char **) * count);
			argv = (char **)malloc(sizeof(char **) * count);
			for(int n = 0; n < count; ++n)
			{
				QByteArray cs = args[n].toLocal8Bit();
				data[n] = (char *)qstrdup(cs.data());
				argv[n] = data[n];
			}
			argc = count;
		}
	}

private:
	int count;
	char **data;
};

static void loadPlugins(const QString &pluginPath, bool print = false)
{
	if(print)
		printf("Loading plugins in [%s]\n", qPrintable(pluginPath));
	QDir dir(pluginPath);
	QStringList entryList = dir.entryList(QDir::Files);
	foreach(QString entry, entryList)
	{
		if(!QLibrary::isLibrary(entry))
			continue;
		QString filePath = dir.filePath(entry);
		GError *err = 0;
		GstPlugin *plugin = gst_plugin_load_file(
			filePath.toLatin1().data(), &err);
		if(!plugin)
		{
			if(print)
			{
				printf("**FAIL**: %s: %s\n", qPrintable(entry),
					err->message);
			}
			g_error_free(err);
			continue;
		}
		if(print)
		{
			printf("   OK   : %s name=[%s]\n", qPrintable(entry),
				gst_plugin_get_name(plugin));
		}
		gst_object_unref(plugin);
	}

	if(print)
		printf("\n");
}

class GstSession
{
public:
	CArgs args;
	QString version;

	GstSession(const QString &pluginPath = QString())
	{
		args.set(QCoreApplication::instance()->arguments());

		// make sure glib threads are available
		if(!g_thread_supported())
			g_thread_init(NULL);

		// you can also use NULLs here if you don't want to pass args
		gst_init(&args.argc, &args.argv);

		guint major, minor, micro, nano;
		gst_version(&major, &minor, &micro, &nano);

		QString nano_str;
		if(nano == 1)
			nano_str = " (CVS)";
		else if(nano == 2)
			nano_str = " (Prerelease)";

		version.sprintf("%d.%d.%d%s", major, minor, micro,
			!nano_str.isEmpty() ? qPrintable(nano_str) : "");

		// manually load plugins?
		if(!pluginPath.isEmpty())
			loadPlugins(pluginPath);

		gstcustomelements_register();
	}

	~GstSession()
	{
		// nothing i guess
	}
};

//----------------------------------------------------------------------------
// GstThread
//----------------------------------------------------------------------------
/*static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data)
{
	Q_UNUSED(bus);
	GMainLoop *loop = (GMainLoop *)data;
	switch(GST_MESSAGE_TYPE(msg))
	{
		case GST_MESSAGE_EOS:
		{
			g_print("End-of-stream\n");
			g_main_loop_quit(loop);
			break;
		}
		case GST_MESSAGE_ERROR:
		{
			gchar *debug;
			GError *err;

			gst_message_parse_error(msg, &err, &debug);
			g_free(debug);

			g_print("Error: %s\n", err->message);
			g_error_free(err);

			g_main_loop_quit(loop);
			break;
		}
		default:
			break;
	}

	return TRUE;
}

static void nullAndUnrefElements(QList<GstElement*> &in)
{
	foreach(GstElement *e, in)
	{
		gst_element_set_state(e, GST_STATE_NULL);
		g_object_unref(G_OBJECT(e));
	}
}*/

Q_GLOBAL_STATIC(QMutex, render_mutex)
class GstRtpSessionContext;
static GstRtpSessionContext *g_producer = 0;
static GstRtpSessionContext *g_receiver = 0;
static QList<QImage> *g_images = 0;

class GstRtpChannel;
static void receiver_write(GstRtpChannel *from, const PRtpPacket &rtp);
static QList<QImage> *g_rimages = 0;

static void gst_show_frame(int width, int height, const unsigned char *rgb24, gpointer appdata)
{
	Q_UNUSED(appdata);

	QImage image(width, height, QImage::Format_RGB32);
	int at = 0;
	for(int y = 0; y < height; ++y)
	{
		for(int x = 0; x < width; ++x)
		{
			unsigned char r = rgb24[at++];
			unsigned char g = rgb24[at++];
			unsigned char b = rgb24[at++];
			QRgb color = qRgb(r, g, b);
			image.setPixel(x, y, color);
		}
	}

	QMutexLocker locker(render_mutex());
	if(!g_images)
		g_images = new QList<QImage>;
	g_images->append(image);
	QMetaObject::invokeMethod((QObject *)g_producer, "imageReady", Qt::QueuedConnection);
}

static void gst_show_rframe(int width, int height, const unsigned char *rgb24, gpointer appdata)
{
	Q_UNUSED(appdata);

	QImage image(width, height, QImage::Format_RGB32);
	int at = 0;
	for(int y = 0; y < height; ++y)
	{
		for(int x = 0; x < width; ++x)
		{
			unsigned char r = rgb24[at++];
			unsigned char g = rgb24[at++];
			unsigned char b = rgb24[at++];
			QRgb color = qRgb(r, g, b);
			image.setPixel(x, y, color);
		}
	}

	QMutexLocker locker(render_mutex());
	if(!g_rimages)
		g_rimages = new QList<QImage>;
	g_rimages->append(image);
	QMetaObject::invokeMethod((QObject *)g_receiver, "rimageReady", Qt::QueuedConnection);
}

Q_GLOBAL_STATIC(QMutex, in_mutex)
static QList<PRtpPacket> *g_in_packets_audio = 0;
static QList<PRtpPacket> *g_in_packets = 0;
static int eat_audio = 0;//1000;

static void gst_packet_ready_audio(const unsigned char *buf, int size, gpointer data)
{
	Q_UNUSED(data);

	QMutexLocker locker(in_mutex());
	if(!g_in_packets_audio)
		g_in_packets_audio = new QList<PRtpPacket>();
	if(eat_audio > 0)
	{
		--eat_audio;
		if(eat_audio == 0)
			printf("done eating packets\n");
		return;
	}
	QByteArray ba((const char *)buf, size);
	PRtpPacket packet;
	packet.rawValue = ba;
	packet.portOffset = 0;
	if(g_in_packets_audio->count() < 5)
	{
		g_in_packets_audio->append(packet);
		QMetaObject::invokeMethod((QObject *)g_producer, "packetReadyAudio", Qt::QueuedConnection);
	}
}

static void gst_packet_ready(const unsigned char *buf, int size, gpointer data)
{
	Q_UNUSED(data);

	QMutexLocker locker(in_mutex());
	if(!g_in_packets)
		g_in_packets = new QList<PRtpPacket>();
	QByteArray ba((const char *)buf, size);
	PRtpPacket packet;
	packet.rawValue = ba;
	packet.portOffset = 0;
	if(g_in_packets->count() < 5)
	{
		g_in_packets->append(packet);
		QMetaObject::invokeMethod((QObject *)g_producer, "packetReady", Qt::QueuedConnection);
	}
}

class GstThread : public QThread
{
	Q_OBJECT

public:
	QString pluginPath;
	GstSession *gstSession;
	GMainContext *mainContext;
	GMainLoop *mainLoop;
	QMutex m;
	QWaitCondition w;
	static GstThread *self;

	GstThread(QObject *parent = 0) :
		QThread(parent),
		gstSession(0),
		mainContext(0),
		mainLoop(0),
		pipeline(0),
		rpipeline(0),
		rvpipeline(0)
	{
		self = this;
	}

	~GstThread()
	{
		stop();
		self = 0;
	}

	static GstThread *instance()
	{
		return self;
	}

	QString ain, vin;
	QString infile;

	GstElement *pipeline;
	GstElement *fileSource;
	GstElement *fileDemux;
	GstElement *audioTarget;
	GstElement *videoTarget;

	PPayloadInfo audioPayloadInfo;
	PPayloadInfo videoPayloadInfo;

	QString aout;
	GstElement *rpipeline, *rvpipeline;
	GstElement *audiortpsrc;
	GstElement *videortpsrc;

	PPayloadInfo raudioPayloadInfo;
	PPayloadInfo rvideoPayloadInfo;

	void start(const QString &_pluginPath = QString())
	{
		QMutexLocker locker(&m);
		pluginPath = _pluginPath;
		QThread::start();
		w.wait(&m);
	}

	void stop()
	{
		QMutexLocker locker(&m);
		if(mainLoop)
		{
			// thread-safe ?
			g_main_loop_quit(mainLoop);
			w.wait(&m);
		}

		wait();
	}

	void startProducer()
	{
		GSource *timer = g_timeout_source_new(0);
		g_source_set_callback(timer, cb_doStartProducer, this, NULL);
		g_source_attach(timer, mainContext);
	}

	void startReceiver()
	{
		GSource *timer = g_timeout_source_new(0);
		g_source_set_callback(timer, cb_doStartReceiver, this, NULL);
		g_source_attach(timer, mainContext);
	}

	void stopProducer()
	{
		GSource *timer = g_timeout_source_new(0);
		g_source_set_callback(timer, cb_doStopProducer, this, NULL);
		g_source_attach(timer, mainContext);
	}

	void stopReceiver()
	{
		GSource *timer = g_timeout_source_new(0);
		g_source_set_callback(timer, cb_doStopReceiver, this, NULL);
		g_source_attach(timer, mainContext);
	}

signals:
	void producer_started();
	void producer_stopped();
	void producer_finished();
	void producer_error();

	void receiver_started();
	void receiver_stopped();

protected:
	virtual void run()
	{
		printf("GStreamer thread started\n");

		 // this will be unlocked as soon as the mainloop runs
		m.lock();

		gstSession = new GstSession(pluginPath);
		printf("Using GStreamer version %s\n", qPrintable(gstSession->version));

		mainContext = g_main_context_new();
		mainLoop = g_main_loop_new(mainContext, FALSE);

		// deferred call to loop_started()
		GSource *timer = g_timeout_source_new(0);
		g_source_attach(timer, mainContext);
		g_source_set_callback(timer, cb_loop_started, this, NULL);

		// kick off the event loop
		g_main_loop_run(mainLoop);

		QMutexLocker locker(&m);

		// cleanup
		cleanup_producer();

		g_main_loop_unref(mainLoop);
		mainLoop = 0;
		g_main_context_unref(mainContext);
		mainContext = 0;
		delete gstSession;
		gstSession = 0;

		w.wakeOne();
		printf("GStreamer thread completed\n");
	}

private:
	static gboolean cb_loop_started(gpointer data)
	{
		return ((GstThread *)data)->loop_started();
	}

	static gboolean cb_doStartProducer(gpointer data)
	{
		return ((GstThread *)data)->doStartProducer();
	}

	static gboolean cb_doStartReceiver(gpointer data)
	{
		return ((GstThread *)data)->doStartReceiver();
	}

	static gboolean cb_doStopProducer(gpointer data)
	{
		return ((GstThread *)data)->doStopProducer();
	}

	static gboolean cb_doStopReceiver(gpointer data)
	{
		return ((GstThread *)data)->doStopReceiver();
	}

	static void cb_fileDemux_pad_added(GstElement *element, GstPad *pad, gpointer data)
	{
		((GstThread *)data)->fileDemux_pad_added(element, pad);
	}

	static void cb_fileDemux_pad_removed(GstElement *element, GstPad *pad, gpointer data)
	{
		((GstThread *)data)->fileDemux_pad_removed(element, pad);
	}

	gboolean loop_started()
	{
		w.wakeOne();
		m.unlock();
		return FALSE;
	}

	gboolean doStartProducer()
	{
		pipeline = gst_pipeline_new(NULL);

		GstElement *audioin = 0;
		GstElement *videoin = 0;
		fileSource = 0;
		GstCaps *videoincaps = 0;

		if(!infile.isEmpty())
		{
			printf("creating filesrc\n");

			fileSource = gst_element_factory_make("filesrc", NULL);
			g_object_set(G_OBJECT(fileSource), "location", infile.toLatin1().data(), NULL);

			fileDemux = gst_element_factory_make("oggdemux", NULL);
			g_signal_connect(G_OBJECT(fileDemux),
				"pad-added",
				G_CALLBACK(cb_fileDemux_pad_added), this);
			g_signal_connect(G_OBJECT(fileDemux),
				"pad-removed",
				G_CALLBACK(cb_fileDemux_pad_removed), this);
		}
		else
		{
			if(!ain.isEmpty())
			{
				printf("creating audioin\n");

				audioin = make_device_element(ain, PDevice::AudioIn);
				if(!audioin)
				{
					// TODO
					printf("failed to create audio input element\n");
				}
			}

			if(!vin.isEmpty())
			{
				printf("creating videoin\n");

				QSize captureSize;
				videoin = make_device_element(vin, PDevice::VideoIn, &captureSize);
				if(!videoin)
				{
					// TODO
					printf("failed to create video input element\n");
				}

				if(captureSize.isValid())
				{
					videoincaps = gst_caps_new_simple("video/x-raw-yuv",
						"width", G_TYPE_INT, captureSize.width(),
						"height", G_TYPE_INT, captureSize.height(), NULL);
				}
			}
		}

		GstElement *audioqueue = 0, *audioconvert = 0, *audioresample = 0, *audioenc = 0, *audiortppay = 0, *audiortpsink = 0;
		GstAppRtpSink *appRtpSink = 0;
		GstCaps *caps5 = 0;

		if(audioin || fileSource)
		{
			audioqueue = gst_element_factory_make("queue", NULL);
			audioconvert = gst_element_factory_make("audioconvert", NULL);
			audioresample = gst_element_factory_make("audioresample", NULL);
			audioenc = gst_element_factory_make("speexenc", NULL);
			audiortppay = gst_element_factory_make("rtpspeexpay", NULL);
			//g_object_set(G_OBJECT(audiortppay), "min-ptime", (guint64)1000000000, NULL);
			//audiosink = gst_element_factory_make("alsasink", NULL);
			audiortpsink = gst_element_factory_make("apprtpsink", NULL);
			appRtpSink = (GstAppRtpSink *)audiortpsink;
			appRtpSink->packet_ready = gst_packet_ready_audio;

			caps5 = gst_caps_new_simple("audio/x-raw-int",
				"rate", G_TYPE_INT, 16000,
				"channels", G_TYPE_INT, 1, NULL);
		}

		GstElement *videoqueue = 0, *videoconvert = 0, *videosink = 0;
		GstAppVideoSink *appVideoSink = 0;
		GstElement *videoconvertpre = 0, *videotee = 0, *videortpqueue = 0, *videoenc = 0, *videortppay = 0, *videortpsink = 0;

		if(videoin || fileSource)
		{
			videoqueue = gst_element_factory_make("queue", NULL);
			videoconvert = gst_element_factory_make("ffmpegcolorspace", NULL);
			//videosink = gst_element_factory_make("ximagesink", NULL);
			videosink = gst_element_factory_make("appvideosink", NULL);
			if(!videosink)
			{
				printf("could not make videosink!!\n");
			}
			appVideoSink = (GstAppVideoSink *)videosink;
			appVideoSink->show_frame = gst_show_frame;
			//g_object_set(G_OBJECT(appVideoSink), "sync", FALSE, NULL);

			videoconvertpre = gst_element_factory_make("ffmpegcolorspace", NULL);
			videotee = gst_element_factory_make("tee", NULL);
			videortpqueue = gst_element_factory_make("queue", NULL);
			videoenc = gst_element_factory_make("theoraenc", NULL);
			videortppay = gst_element_factory_make("rtptheorapay", NULL);
			videortpsink = gst_element_factory_make("apprtpsink", NULL);
			if(!videotee || !videortpqueue || !videoenc || !videortppay || !videortpsink)
				printf("error making some video stuff\n");
			appRtpSink = (GstAppRtpSink *)videortpsink;
			appRtpSink->packet_ready = gst_packet_ready;
		}

		if(audioin)
			gst_bin_add(GST_BIN(pipeline), audioin);
		if(videoin)
			gst_bin_add(GST_BIN(pipeline), videoin);

		if(fileSource)
			gst_bin_add_many(GST_BIN(pipeline), fileSource, fileDemux, NULL);

		if(audioin || fileSource)
			gst_bin_add_many(GST_BIN(pipeline), audioqueue, audioconvert, audioresample, audioenc, audiortppay, audiortpsink, NULL);
		if(videoin || fileSource)
		{
			gst_bin_add_many(GST_BIN(pipeline), videoconvertpre, videotee, videoqueue, videoconvert, videosink, NULL);
			gst_bin_add_many(GST_BIN(pipeline), videortpqueue, videoenc, videortppay, videortpsink, NULL);
		}

		if(fileSource)
			gst_element_link_many(fileSource, fileDemux, NULL);

		if(audioin || fileSource)
		{
			gst_element_link_many(audioqueue, audioconvert, audioresample, NULL);
			gst_element_link_filtered(audioresample, audioenc, caps5);
			gst_element_link_many(audioenc, audiortppay, audiortpsink, NULL);
		}
		if(videoin || fileSource)
		{
			gst_element_link_many(videoconvertpre, videotee, videoqueue, videoconvert, videosink, NULL);
			gst_element_link_many(videotee, videortpqueue, videoenc, videortppay, videortpsink, NULL);
		}

		if(audioin || fileSource)
			audioTarget = audioqueue;
		if(videoin || fileSource)
			videoTarget = videoconvertpre;

		if(audioin)
			gst_element_link_many(audioin, audioTarget, NULL);
		if(videoin)
		{
			if(videoincaps)
				gst_element_link_filtered(videoin, videoTarget, videoincaps);
			else
				gst_element_link(videoin, videoTarget);
		}

		//GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(e_pipeline));
		//gst_bus_add_watch(bus, bus_call, loop);
		//gst_object_unref(bus);

		// ### seems for live streams we need playing state to get caps.
		//   paused may not be enough??
		gst_element_set_state(pipeline, GST_STATE_PLAYING);
		gst_element_get_state(pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);

		if(audioin || fileSource)
		{
			GstPad *pad = gst_element_get_pad(audiortppay, "src");
			GstCaps *caps = gst_pad_get_negotiated_caps(pad);
			gchar *gstr = gst_caps_to_string(caps);
			QString capsString = QString::fromUtf8(gstr);
			g_free(gstr);
			printf("rtppay caps audio: [%s]\n", qPrintable(capsString));
			g_object_unref(pad);

			GstStructure *cs = gst_caps_get_structure(caps, 0);

			audioPayloadInfo = structureToPayloadInfo(cs);
			if(audioPayloadInfo.id == -1)
			{
				// TODO: handle error
			}

			gst_caps_unref(caps);
		}

		if(videoin || fileSource)
		{
			GstPad *pad = gst_element_get_pad(videortppay, "src");
			GstCaps *caps = gst_pad_get_negotiated_caps(pad);
			gchar *gstr = gst_caps_to_string(caps);
			QString capsString = QString::fromUtf8(gstr);
			g_free(gstr);
			printf("rtppay caps video: [%s]\n", qPrintable(capsString));
			gst_object_unref(pad);

			GstStructure *cs = gst_caps_get_structure(caps, 0);

			videoPayloadInfo = structureToPayloadInfo(cs);
			if(videoPayloadInfo.id == -1)
			{
				// TODO: handle error
			}

			gst_caps_unref(caps);
		}

		//gst_element_set_state(pipeline, GST_STATE_PLAYING);
		//gst_element_get_state(pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);

		emit producer_started();

		return FALSE;
	}

	gboolean doStopProducer()
	{
		cleanup_producer();

		emit producer_stopped();

		return FALSE;
	}

	gboolean doStopReceiver()
	{
		cleanup_receiver();

		emit receiver_stopped();

		return FALSE;
	}

	void cleanup_producer()
	{
		if(!pipeline)
			return;

		gst_element_set_state(pipeline, GST_STATE_NULL);
		gst_element_get_state(pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);

		gst_object_unref(GST_OBJECT(pipeline));
		pipeline = 0;
	}

	void cleanup_receiver()
	{
		if(rpipeline)
		{
			gst_element_set_state(rpipeline, GST_STATE_NULL);
			gst_element_get_state(rpipeline, NULL, NULL, GST_CLOCK_TIME_NONE);

			gst_object_unref(GST_OBJECT(rpipeline));
			rpipeline = 0;
		}

		if(rvpipeline)
		{
			gst_element_set_state(rvpipeline, GST_STATE_NULL);
			gst_element_get_state(rvpipeline, NULL, NULL, GST_CLOCK_TIME_NONE);

			gst_object_unref(GST_OBJECT(rvpipeline));
			rvpipeline = 0;
		}
	}

	void fileDemux_pad_added(GstElement *element, GstPad *pad)
	{
		Q_UNUSED(element);

		gchar *name = gst_pad_get_name(pad);
		printf("pad-added: %s\n", name);
		g_free(name);

		GstCaps *caps = gst_pad_get_caps(pad);
		gchar *gstr = gst_caps_to_string(caps);
		QString capsString = QString::fromUtf8(gstr);
		g_free(gstr);
		printf("  caps: [%s]\n", qPrintable(capsString));

		int num = gst_caps_get_size(caps);
		for(int n = 0; n < num; ++n)
		{
			GstStructure *cs = gst_caps_get_structure(caps, n);
			QString mime = gst_structure_get_name(cs);

			QStringList parts = mime.split('/');
			if(parts.count() != 2)
				continue;
			QString type = parts[0];
			QString subtype = parts[1];

			GstElement *decoder = 0;
			GstElement *target = 0;

			// FIXME: in the future, we should probably do this
			//   more dynamically, by inspecting the pads on the
			//   decoder and comparing to the source pad, rather
			//   than assuming fixed values (like 'x-speex').
			if(type == "audio")
			{
				target = audioTarget;

				if(subtype == "x-speex")
					decoder = gst_element_factory_make("speexdec", NULL);
				else if(subtype == "x-vorbis")
					decoder = gst_element_factory_make("vorbisdec", NULL);
			}
			else if(type == "video")
			{
				target = videoTarget;

				if(subtype == "x-theora")
					decoder = gst_element_factory_make("theoradec", NULL);
			}

			if(decoder)
			{
				if(!gst_bin_add(GST_BIN(pipeline), decoder))
					continue;
				GstPad *sinkpad = gst_element_get_static_pad(decoder, "sink");
				if(!GST_PAD_LINK_SUCCESSFUL(gst_pad_link(pad, sinkpad)))
					continue;
				gst_object_unref(sinkpad);

				GstPad *sourcepad = gst_element_get_static_pad(decoder, "src");
				sinkpad = gst_element_get_static_pad(target, "sink");
				if(!GST_PAD_LINK_SUCCESSFUL(gst_pad_link(sourcepad, sinkpad)))
					continue;
				gst_object_unref(sourcepad);
				gst_object_unref(sinkpad);

				// by default the element is not in a working state.
				//   we set to 'paused' which hopefully means it'll
				//   do the right thing.
				gst_element_set_state(decoder, GST_STATE_PAUSED);

				// decoder set up, we're done
				break;
			}
		}

		gst_caps_unref(caps);
	}

	void fileDemux_pad_removed(GstElement *element, GstPad *pad)
	{
		Q_UNUSED(element);

		// TODO

		gchar *name = gst_pad_get_name(pad);
		printf("pad-removed: %s\n", name);
		g_free(name);
	}

	gboolean doStartReceiver()
	{
		rpipeline = gst_pipeline_new(NULL);
		rvpipeline = gst_pipeline_new(NULL);

#ifdef UDP_LOOPBACK
		audiortpsrc = gst_element_factory_make("udpsrc", NULL);
		g_object_set(G_OBJECT(audiortpsrc), "port", 61000, NULL);
#else
		audiortpsrc = gst_element_factory_make("apprtpsrc", NULL);
#endif

		//GstStructure *cs = gst_structure_from_string("application/x-rtp, media=(string)audio, clock-rate=(int)16000, encoding-name=(string)SPEEX, encoding-params=(string)1, payload=(int)110", NULL);
		//GstStructure *cs = gst_structure_from_string("application/x-rtp, media=(string)audio, clock-rate=(int)8000, encoding-name=(string)PCMU, payload=(int)0", NULL);
		GstStructure *cs = payloadInfoToStructure(raudioPayloadInfo, "audio");
		if(!cs)
		{
			// TODO: handle error
			printf("cannot parse payload info\n");
		}

		GstCaps *caps = gst_caps_new_empty();
		gst_caps_append_structure(caps, cs);
		g_object_set(G_OBJECT(audiortpsrc), "caps", caps, NULL);
		gst_caps_unref(caps);

#ifdef UDP_LOOPBACK
		videortpsrc = gst_element_factory_make("udpsrc", NULL);
		g_object_set(G_OBJECT(videortpsrc), "port", 61002, NULL);
#else
		videortpsrc = gst_element_factory_make("apprtpsrc", NULL);
#endif
		cs = payloadInfoToStructure(rvideoPayloadInfo, "video");
		if(!cs)
		{
			// TODO: handle error
			printf("cannot parse payload info\n");
		}

		caps = gst_caps_new_empty();
		gst_caps_append_structure(caps, cs);
		g_object_set(G_OBJECT(videortpsrc), "caps", caps, NULL);
		gst_caps_unref(caps);

		//GstElement *audioqueue = gst_element_factory_make("queue", NULL);
		GstElement *audiortpjitterbuffer = gst_element_factory_make("gstrtpjitterbuffer", NULL);
		GstElement *audiortpdepay = gst_element_factory_make("rtpspeexdepay", NULL);
		GstElement *audiodec = gst_element_factory_make("speexdec", NULL);
		GstElement *audioconvert = gst_element_factory_make("audioconvert", NULL);
		GstElement *audioresample = gst_element_factory_make("audioresample", NULL);
		GstElement *audioout = 0;

		if(audiortpjitterbuffer)
		{
			gst_bin_add_many(GST_BIN(rpipeline), audiortpsrc, audiortpjitterbuffer, audiortpdepay, audiodec, audioconvert, audioresample, NULL);
			gst_element_link_many(audiortpsrc, audiortpjitterbuffer, audiortpdepay, audiodec, audioconvert, audioresample, NULL);
			g_object_set(G_OBJECT(audiortpjitterbuffer), "latency", (unsigned int)400, NULL);
		}
		else
		{
			gst_bin_add_many(GST_BIN(rpipeline), audiortpsrc, audiortpdepay, audiodec, audioconvert, audioresample, NULL);
			gst_element_link_many(audiortpsrc, audiortpdepay, audiodec, audioconvert, audioresample, NULL);
		}

		if(!aout.isEmpty())
		{
			printf("creating audioout\n");

			audioout = make_device_element(aout, PDevice::AudioOut);
			if(!audioout)
			{
				// TODO
				printf("failed to create audio output element\n");
			}
		}
		else
			audioout = gst_element_factory_make("fakesink", NULL);

		//GstElement *videoqueue = gst_element_factory_make("queue", NULL);
		GstElement *videortpjitterbuffer = gst_element_factory_make("gstrtpjitterbuffer", NULL);
		GstElement *videortpdepay = gst_element_factory_make("rtptheoradepay", NULL);
		GstElement *videodec = gst_element_factory_make("theoradec", NULL);
		GstElement *videoconvert = gst_element_factory_make("ffmpegcolorspace", NULL);
		//GstElement *videosink = gst_element_factory_make("ximagesink", NULL);
		GstElement *videosink = gst_element_factory_make("appvideosink", NULL);
		if(!videosink)
		{
			printf("could not make videosink!!\n");
		}
		GstAppVideoSink *appVideoSink = (GstAppVideoSink *)videosink;
		appVideoSink->show_frame = gst_show_rframe;

		if(videortpjitterbuffer)
		{
			gst_bin_add_many(GST_BIN(rvpipeline), videortpsrc, /*videoqueue,*/ videortpjitterbuffer, videortpdepay, videodec, videoconvert, videosink, NULL);
			gst_element_link_many(videortpsrc, /*videoqueue,*/ videortpjitterbuffer, videortpdepay, videodec, videoconvert, videosink, NULL);
			g_object_set(G_OBJECT(audiortpjitterbuffer), "latency", (unsigned int)400, NULL);
		}
		else
		{
			gst_bin_add_many(GST_BIN(rvpipeline), videortpsrc, /*videoqueue,*/ videortpdepay, videodec, videoconvert, videosink, NULL);
			gst_element_link_many(videortpsrc, /*videoqueue,*/ videortpdepay, videodec, videoconvert, videosink, NULL);
		}

		gst_bin_add(GST_BIN(rpipeline), audioout);
		gst_element_link_many(audioresample, audioout, NULL);

		gst_element_set_state(rpipeline, GST_STATE_READY);
		gst_element_get_state(rpipeline, NULL, NULL, GST_CLOCK_TIME_NONE);

		gst_element_set_state(rvpipeline, GST_STATE_READY);
		gst_element_get_state(rvpipeline, NULL, NULL, GST_CLOCK_TIME_NONE);

		gst_element_set_state(rpipeline, GST_STATE_PLAYING);

		gst_element_set_state(rvpipeline, GST_STATE_PLAYING);

		printf("receive pipeline started\n");

		emit receiver_started();

		return FALSE;
	}
};

GstThread *GstThread::self = 0;

//----------------------------------------------------------------------------
// GstRtpChannel
//----------------------------------------------------------------------------
class GstProducerContext;

class GstRtpChannel : public QObject, public RtpChannelContext
{
	Q_OBJECT
	Q_INTERFACES(PsiMedia::RtpChannelContext)

public:
	friend class GstRtpSessionContext;
	QList<PRtpPacket> in;

	virtual QObject *qobject()
	{
		return this;
	}

	virtual void setEnabled(bool b)
	{
		// TODO
		Q_UNUSED(b);
	}

	virtual int packetsAvailable() const
	{
		return in.count();
	}

	virtual PRtpPacket read()
	{
		return in.takeFirst();
	}

	virtual void write(const PRtpPacket &rtp)
	{
		// TODO
		receiver_write(this, rtp);
	}

signals:
	void readyRead();
	void packetsWritten(int count);
};

//----------------------------------------------------------------------------
// GstRtpSessionContext
//----------------------------------------------------------------------------
class GstRtpSessionContext : public QObject, public RtpSessionContext
{
	Q_OBJECT
	Q_INTERFACES(PsiMedia::RtpSessionContext)

public:
	QString audioOutId;
	QString audioInId, videoInId;
	QString fileIn;
	QByteArray fileDataIn;
	VideoWidgetContext *outputWidget, *previewWidget;
	int audioOutVolume;
	int audioInVolume;
	int code;

	GstRtpChannel audioRtp;
	GstRtpChannel videoRtp;

	// FIXME: remove this
	bool producerMode;

	GstRtpSessionContext(QObject *parent = 0) :
		QObject(parent),
		outputWidget(0),
		previewWidget(0),
		audioOutVolume(100),
		audioInVolume(100),
		code(-1)
	{
	}

	virtual QObject *qobject()
	{
		return this;
	}

	virtual void setAudioOutputDevice(const QString &deviceId)
	{
		audioOutId = deviceId;
		// TODO: if active, switch to that device
	}

	virtual void setAudioInputDevice(const QString &deviceId)
	{
		audioInId = deviceId;
		// TODO: if active, switch to that device
	}

	virtual void setVideoInputDevice(const QString &deviceId)
	{
		videoInId = deviceId;
		// TODO: if active, switch to that device
	}

	virtual void setFileInput(const QString &fileName)
	{
		fileIn = fileName;
		// TODO: if active, switch to playing this file
	}

	virtual void setFileDataInput(const QByteArray &fileData)
	{
		fileDataIn = fileData;
		// TODO: if active, switch to playing this file data
	}

#ifdef QT_GUI_LIB
        virtual void setVideoOutputWidget(VideoWidgetContext *widget)
	{
		outputWidget = widget;
		// TODO: if active, switch to using (or not using)
	}

	virtual void setVideoPreviewWidget(VideoWidgetContext *widget)
	{
		previewWidget = widget;
		// TODO: if active, switch to using (or not using)
	}
#endif

	virtual void setRecorder(QIODevice *recordDevice)
	{
		// TODO
		Q_UNUSED(recordDevice);
	}

	virtual void setLocalAudioPreferences(const QList<PAudioParams> &params)
	{
		// TODO
		Q_UNUSED(params);
	}

	virtual void setLocalAudioPreferences(const QList<PPayloadInfo> &info)
	{
		// TODO
		Q_UNUSED(info);
	}

	virtual void setLocalVideoPreferences(const QList<PVideoParams> &params)
	{
		// TODO
		Q_UNUSED(params);
	}

	virtual void setLocalVideoPreferences(const QList<PPayloadInfo> &info)
	{
		// TODO
		Q_UNUSED(info);
	}

	virtual void setRemoteAudioPreferences(const QList<PPayloadInfo> &info)
	{
		// TODO
		GstThread::instance()->raudioPayloadInfo = info.first();
	}

	virtual void setRemoteVideoPreferences(const QList<PPayloadInfo> &info)
	{
		// TODO
		GstThread::instance()->rvideoPayloadInfo = info.first();
	}

	virtual void start()
	{
		// TODO

		// probably producer
		if(!audioInId.isEmpty() || !videoInId.isEmpty() || !fileIn.isEmpty())
		{
			producerMode = true;
			g_producer = this;

			connect(GstThread::instance(), SIGNAL(producer_started()), SIGNAL(started()));
			connect(GstThread::instance(), SIGNAL(producer_stopped()), SIGNAL(stopped()));

			GstThread::instance()->ain = audioInId;
			GstThread::instance()->vin = videoInId;
			GstThread::instance()->infile = fileIn;
			GstThread::instance()->startProducer();
		}
		// receiver
		else
		{
			producerMode = false;
			g_receiver = this;

			connect(GstThread::instance(), SIGNAL(receiver_started()), SIGNAL(started()));
			connect(GstThread::instance(), SIGNAL(receiver_stopped()), SIGNAL(stopped()));

			GstThread::instance()->aout = audioOutId;
			GstThread::instance()->startReceiver();
		}
	}

	virtual void updatePreferences()
	{
		// TODO
	}

	virtual void transmitAudio(int index)
	{
		// TODO (note that -1 means pick best)
		Q_UNUSED(index);
	}

	virtual void transmitVideo(int index)
	{
		// TODO (note that -1 means pick best)
		Q_UNUSED(index);
	}

	virtual void pauseAudio()
	{
		// TODO
	}

	virtual void pauseVideo()
	{
		// TODO
	}

	virtual void stop()
	{
		// TODO

		if(producerMode)
			GstThread::instance()->stopProducer();
		else
			GstThread::instance()->stopReceiver();
	}

	virtual QList<PPayloadInfo> audioPayloadInfo() const
	{
		// TODO
		return QList<PPayloadInfo>() << GstThread::instance()->audioPayloadInfo;
	}

	virtual QList<PPayloadInfo> videoPayloadInfo() const
	{
		// TODO
		return QList<PPayloadInfo>() << GstThread::instance()->videoPayloadInfo;
	}

	virtual QList<PAudioParams> audioParams() const
	{
		// TODO
		return QList<PAudioParams>();
	}

	virtual QList<PVideoParams> videoParams() const
	{
		// TODO
		return QList<PVideoParams>();
	}

	virtual int outputVolume() const
	{
		return audioOutVolume;
	}

	virtual void setOutputVolume(int level)
	{
		audioOutVolume = level;
		// TODO: if active, change active volume
	}

	virtual int inputVolume() const
	{
		return audioInVolume;
	}

	virtual void setInputVolume(int level)
	{
		audioInVolume = level;
		// TODO: if active, change active volume
	}

	virtual Error errorCode() const
	{
		return (Error)code;
	}

	virtual RtpChannelContext *audioRtpChannel()
	{
		return &audioRtp;
	}

	virtual RtpChannelContext *videoRtpChannel()
	{
		return &videoRtp;
	}

signals:
	void started();
	void preferencesUpdated();
	void stopped();
	void finished();
	void error();

public:
	void doWrite(GstRtpChannel *from, const PRtpPacket &rtp)
	{
		if(from == &audioRtp && rtp.portOffset == 0)
		{
#ifdef UDP_LOOPBACK
			audioloop->writeDatagram(rtp.rawValue, QHostAddress("127.0.0.1"), 61000);
#else
			GstAppRtpSrc *src = (GstAppRtpSrc *)GstThread::instance()->audiortpsrc;
			gst_apprtpsrc_packet_push(src, (const unsigned char *)rtp.rawValue.data(), rtp.rawValue.size());
#endif
		}
		else if(from == &videoRtp && rtp.portOffset == 0)
		{
#ifdef UDP_LOOPBACK
			videoloop->writeDatagram(rtp.rawValue, QHostAddress("127.0.0.1"), 61002);
#else
			GstAppRtpSrc *src = (GstAppRtpSrc *)GstThread::instance()->videortpsrc;
			gst_apprtpsrc_packet_push(src, (const unsigned char *)rtp.rawValue.data(), rtp.rawValue.size());
#endif
		}
	}

public slots:
	void imageReady()
	{
		render_mutex()->lock();
		QImage image = g_images->takeFirst();
		render_mutex()->unlock();

		if(previewWidget)
			previewWidget->show_frame(image);
	}

	void packetReadyAudio()
	{
		in_mutex()->lock();
		PRtpPacket packet = g_in_packets_audio->takeFirst();
		in_mutex()->unlock();

		//printf("audio packet ready (%d bytes)\n", packet.rawValue.size());
		audioRtp.in += packet;
		emit audioRtp.readyRead();
	}

	void packetReady()
	{
		in_mutex()->lock();
		PRtpPacket packet = g_in_packets->takeFirst();
		in_mutex()->unlock();

		//printf("video packet ready\n");
		videoRtp.in += packet;
		emit videoRtp.readyRead();
	}

	void rimageReady()
	{
		render_mutex()->lock();
		QImage image = g_rimages->takeFirst();
		render_mutex()->unlock();

		if(outputWidget)
			outputWidget->show_frame(image);
	}
};

void receiver_write(GstRtpChannel *from, const PRtpPacket &rtp)
{
	if(g_receiver)
		g_receiver->doWrite(from, rtp);
}

//----------------------------------------------------------------------------
// GstProvider
//----------------------------------------------------------------------------
class GstProvider : public QObject, public Provider
{
	Q_OBJECT
	Q_INTERFACES(PsiMedia::Provider)

public:
	GstThread *thread;

	GstProvider() :
		thread(0)
	{
	}

	virtual QObject *qobject()
	{
		return this;
	}

	virtual bool init(const QString &resourcePath)
	{
		thread = new GstThread(this);
		thread->start(resourcePath);
		return true;
	}

	~GstProvider()
	{
		delete thread;
	}

	virtual QString creditName()
	{
		return "GStreamer";
	}

	virtual QString creditText()
	{
		QString str =
		"This application uses GStreamer, a comprehensive "
		"open-source and cross-platform multimedia framework.  For "
		"more information, see http://www.gstreamer.net/\n\n"
		"If you enjoy this software, please give the GStreamer "
		"people a million dollars.";
		return str;
	}

	// FIXME: any better way besides hardcoding?
	virtual QList<PAudioParams> supportedAudioModes()
	{
		QList<PAudioParams> list;
		{
			PAudioParams p;
			p.codec = "speex";
			p.sampleRate = 8000;
			p.sampleSize = 16;
			p.channels = 1;
			list += p;
		}
		{
			PAudioParams p;
			p.codec = "speex";
			p.sampleRate = 16000;
			p.sampleSize = 16;
			p.channels = 1;
			list += p;
		}
		{
			PAudioParams p;
			p.codec = "speex";
			p.sampleRate = 32000;
			p.sampleSize = 16;
			p.channels = 1;
			list += p;
		}
		{
			PAudioParams p;
			p.codec = "vorbis";
			p.sampleRate = 44100;
			p.sampleSize = 16;
			p.channels = 2;
			list += p;
		}
		return list;
	}

	// FIXME: any better way besides hardcoding?
	virtual QList<PVideoParams> supportedVideoModes()
	{
		QList<PVideoParams> list;
		{
			PVideoParams p;
			p.codec = "theora";
			p.size = QSize(160, 120);
			p.fps = 15;
			list += p;
		}
		{
			PVideoParams p;
			p.codec = "theora";
			p.size = QSize(320, 240);
			p.fps = 15;
			list += p;
		}
		{
			PVideoParams p;
			p.codec = "theora";
			p.size = QSize(320, 240);
			p.fps = 30;
			list += p;
		}
		{
			PVideoParams p;
			p.codec = "theora";
			p.size = QSize(640, 480);
			p.fps = 15;
			list += p;
		}
		{
			PVideoParams p;
			p.codec = "theora";
			p.size = QSize(640, 480);
			p.fps = 30;
			list += p;
		}
		return list;
	}

	virtual QList<PDevice> audioOutputDevices()
	{
		QList<PDevice> list;
		foreach(const GstDevice &i, gstAudioOutputDevices())
			list += gstDeviceToPDevice(i, PDevice::AudioOut);
		return list;
	}

	virtual QList<PDevice> audioInputDevices()
	{
		QList<PDevice> list;
		foreach(const GstDevice &i, gstAudioInputDevices())
			list += gstDeviceToPDevice(i, PDevice::AudioIn);
		return list;
	}

	virtual QList<PDevice> videoInputDevices()
	{
		QList<PDevice> list;
		foreach(const GstDevice &i, gstVideoInputDevices())
			list += gstDeviceToPDevice(i, PDevice::VideoIn);
		return list;
	}

	virtual RtpSessionContext *createRtpSession()
	{
		return new GstRtpSessionContext;
	}
};

class GstPlugin : public QObject, public Plugin
{
	Q_OBJECT
	Q_INTERFACES(PsiMedia::Plugin)

public:
	virtual Provider *createProvider() { return new GstProvider; }
};

}

Q_EXPORT_PLUGIN2(gstprovider, PsiMedia::GstPlugin)

#include "gstprovider.moc"