import java.io.*;
import java.util.*;
import java.net.*;
import java.security.*;
import java.math.*;
import java.lang.Math.*;
import com.xuggle.mediatool.IMediaTool;
import com.xuggle.mediatool.ToolFactory;
import com.xuggle.mediatool.IMediaReader;
import com.xuggle.mediatool.IMediaWriter;
import com.xuggle.mediatool.MediaToolAdapter;
import com.xuggle.mediatool.event.IAudioSamplesEvent;
import com.xuggle.mediatool.event.IVideoPictureEvent;
import com.xuggle.xuggler.IContainer;
import com.xuggle.xuggler.IVideoPicture;
import com.xuggle.xuggler.IStreamCoder;
import com.xuggle.xuggler.IStream;
import com.xuggle.xuggler.ICodec;
import java.util.Map;

public class smilFileNewsService
{
    static final String host = "127.0.0.1";
    String videodir = "";

    public smilFileNewsService() {
        Map<String, String> env = System.getenv();
        videodir = env.get("VIDEODIR");
    }

    /** What is the duration of the video?
     * @param videofile
     * @return duration in seconds
     */
    private long getTotalDuration(String videoFile)
    {
        IContainer container = IContainer.make();
        //videoFile = (String) videoFile.subSequence(0, videoFile.length()-1);
        if (container.open(videoFile, IContainer.Type.READ, null) < 0) {
            System.out.println("Could not open file!  " + videoFile);
        }
        long duration = container.getDuration(); 
        container.close();
        return (duration);
    }

    /** Returns certain video properties of a video
     * @param video
     * @return array containing height and width of the video
     */
    private int[] getVideoProperties(String filename)
    {
        int[] hw = new int[2];

        IContainer container = IContainer.make();

        if (container.open(filename, IContainer.Type.READ, null) < 0)
            throw new IllegalArgumentException("could not open file: " + filename);

        // query how many streams the call to open found

        int numStreams = container.getNumStreams();

        // and iterate through the streams to find the first video stream

        int videoStreamId = -1;
        IStreamCoder videoCoder = null;
        for(int i = 0; i < numStreams; i++) {
            // find the stream object

            IStream stream = container.getStream(i);

            // get the pre-configured decoder that can decode this stream;

            IStreamCoder coder = stream.getStreamCoder();

            if (coder.getCodecType() == ICodec.Type.CODEC_TYPE_VIDEO) {
                videoStreamId = i;
                videoCoder = coder;
                break;
            }
        }

        if (videoStreamId == -1)
            throw new RuntimeException("could not find video stream in container: "+filename);

        if (videoCoder.open() < 0)
            throw new RuntimeException("could not open video decoder for container: " + filename);
        
        hw[0] = videoCoder.getHeight();
        hw[1] = videoCoder.getWidth();

        return (hw);
    }
    /** Create a SMIL file
     * @param Video to be played
     * @param video resolution
     * @param news content
     * @return SMIL file contents
     */
    private String createSmilFile(String mainVideo, int[] hwMain, String news)
    {
        String smil = "";
        String qt = "\"";
        String font = "serif";
        String fontsize = "16px";
        String wrap = "noWrap";
        String bgcolor = "navy";
        String textcolor = "white";
        String style = "mystyle";
        String textregion_2 = "news";
        String wtextregion = "weathertext";
        //String tr_top = "260"; /* tr = textregion */
        //String tr_width = "360";
        String tr_top_main = Integer.toString(hwMain[0] - 25); /* tr = textregion */
        String tr_width_main_1 = Integer.toString(hwMain[1] - 200);
        String tr_width_main_2 = Integer.toString(hwMain[1] - 120);
        String tr_height = "20";
        String tr_left = "10";
        String tr_textmode = "crawl";
        String tr_textrate = "20px";

        smil += "<smil>\n";
        smil += "<head>\n\n";
        smil += "<textStyling>\n";
        
        smil += "<textStyle xml:id="+ qt + style + qt + " textFontFamily=" + qt + font + qt + 
                " textFontSize=" + qt + fontsize + qt + " textWrapOption=" + qt + wrap + qt +
                " textColor=" + qt + textcolor + qt + " textBackgroundColor=" + qt + bgcolor + qt
                + "/>\n";
        
        smil += "</textStyling>\n\n";

        smil += "<layout>\n";

        smil += "<region id=" + qt + textregion_2 + qt + " top=" + qt + tr_top_main + qt + " width=" +
                qt + tr_width_main_2 + qt + " left=" + qt + tr_left + qt + " height=" + 
                qt + tr_height + qt + " textMode=" + qt + tr_textmode + qt +
                " textRate=" + qt + tr_textrate + qt + " textStyle=" + qt + style + qt+ "/>\n";       

        smil += "<region id=" + qt + wtextregion + qt + " top=" + qt + tr_top_main + qt + " width=" +
                qt + tr_width_main_1 + qt + " left=" + qt + tr_left + qt + " height=" + 
                qt + tr_height + qt + "/>\n";       

        smil += "</layout>\n\n";
        smil += "</head>\n\n" +

          "<body><par>\n" +
            "<video src=\"" + mainVideo + "\" />\n" +

            "<smilText xml:id=" + qt + "smiltext_1" + qt + " region=" + qt + textregion_2 + qt +
            " textAlign=" + qt + "right" + qt + " >\n" +
            news +
            "</smilText>\n" +

          "</par>" +
          "</body>\n" +
        "</smil>";

        return (smil);
    }

    /** generate a SMIL file
      * @param video file
      * @return SMIL file contents
      */
    public String getSmilFileWithOverlays(String local)
    {
        String smil = "";
        URL u = null;
        overlayInfoNewsService o = new overlayInfoNewsService();
        int[] hw_main;
        String news = o.getNewsFeed();

        try {
            hw_main = getVideoProperties(new File(videodir).getCanonicalPath() + local);
        } catch (Exception e) {
            hw_main = new int[2];
            hw_main[0] = 500;
            hw_main[1] = 500;

        }

        String localUrl = "http://" + host + ":81" +  local;

        smil = createSmilFile(localUrl, hw_main, news);
        return smil;
    }
}





