//package org.ccnx.ccn.apps.ccnfileproxy;

import java.io.*;
import java.net.*;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.File;
import java.nio.ShortBuffer;

import java.awt.Color;
import java.awt.Font;
import java.awt.Graphics2D;
import java.awt.geom.Rectangle2D;
import java.awt.image.BufferedImage;

import com.xuggle.mediatool.IMediaTool;
import com.xuggle.mediatool.ToolFactory;
import com.xuggle.mediatool.IMediaReader;
import com.xuggle.mediatool.IMediaWriter;
import com.xuggle.mediatool.MediaToolAdapter;
import com.xuggle.mediatool.event.IAudioSamplesEvent;
import com.xuggle.mediatool.event.IVideoPictureEvent;
import com.xuggle.xuggler.IContainer;
import com.xuggle.xuggler.IVideoPicture;

import java.util.Date;
import java.text.DateFormat;
import java.text.SimpleDateFormat;


public class CacheVideo {

	URL videourl = null;
	String cachefile = null;
	static long totalDuration = 0;
	static int prev = 0;
	
	public CacheVideo(String url) {
		cachefile = url;
	    //totalDuration = getTotalDuration(cachefile);
	}
	
	public static void main (String args[]) {
		if (args.length < 1) {
			System.out.println ("No video URL provided.");
			
		} else {
			CacheVideo c = new CacheVideo(args[0]);
			c.processVideo();
		}
	}

	public long getTotalDuration(String videoFile)
	{
		IContainer container = IContainer.make();
		videoFile = (String) videoFile.subSequence(0, videoFile.length());
		if (container.open(videoFile, IContainer.Type.READ, null) < 0) {
			System.out.println("Could not open file!  " + videoFile);
		}
		long duration = container.getDuration(); 
		container.close();
		return (duration);
	}
	
	public void processVideo () {
		String newfile = cachefile.replaceAll(
			".mp4", ".new.mp4");
		
		File inputFile = new File(cachefile);
		if (!inputFile.exists())
		{
		  System.out.println("Input file does not exist: " + inputFile);
		}
		
		File outputFile = new File(newfile);

		// create a media reader and configure it to generate BufferImages
		IMediaReader reader = ToolFactory.makeReader(inputFile.toString());
		reader.setBufferedImageTypeToGenerate(BufferedImage.TYPE_3BYTE_BGR);

		
		// create a writer and configure it's parameters from the reader	
		IMediaWriter writer = ToolFactory.makeWriter(outputFile.toString(), reader);

		// create a tool which paints video time stamp into frame
		IMediaTool addWatermark = new WatermarkTool();

		// create a tool chain:
		reader.addListener(addWatermark);
		addWatermark.addListener(writer);

		// add a viewer to the writer, to see media modified media
		// writer.addListener(ToolFactory.makeViewer());

		// read and decode packets from the source file and
		// then encode and write out data to the output file	
		while (reader.readPacket() == null)
		  ;	
		
		// After processing is completed, overwrite the old file
		//outputFile.write();
		if (!(outputFile.renameTo (inputFile)))
			output ("Unable to overwrite old file: " + inputFile.getName());
	}
	
	public void output (String s) {
		System.out.println ("MicroCDN: " + s);
	}

  /** 
   * Create a tool which adds a time stamp to a video image.
   */

  static class WatermarkTool extends MediaToolAdapter
  {
    public static final String WATERMARK = "NetServ Rocks!";
    overlayInfo o = new overlayInfo();
    public final String WATERMARK1 = o.lookupWeather();
  
    /** {@inheritDoc} */

    public boolean checkTimeStamp(IVideoPictureEvent event)
    {
    	long ptime;
    	IVideoPicture pic = event.getPicture();
    	ptime = pic.getPts();
    	
    	return (ptime < totalDuration/2);
    
    }
    
    public String getWaterMark(IVideoPictureEvent event, String wm)
    {
    	IVideoPicture pic = event.getPicture();
    	long ptime = pic.getPts();
    	int ptimesec = (int) ptime / 1000000;
    	String waterMark;
    	
    	if (wm.length() > totalDuration/1000000) {
    		return (wm);
    	}
    	if (ptimesec*2 > wm.length()) {
    		return (wm);
    	}
    	System.out.println("h = "+pic.getHeight());
    	waterMark = (String)wm.subSequence(0, ptimesec*2);
    	return (waterMark);
    }
    
    @Override
    public void onVideoPicture(IVideoPictureEvent event)
    {
      // get the graphics for the image
      Graphics2D g = event.getImage().createGraphics();
      int fontSize = (int)(event.getImage().getWidth()/25);
	  g.setFont(new Font("LucidaSans", Font.PLAIN, fontSize));
	  
      // establish the timestamp and how much space it will take
      String watermarkStr = WATERMARK;
      Rectangle2D bounds = g.getFont().getStringBounds(watermarkStr,
        g.getFontRenderContext());
      
      // compute the amount to inset the time stamp and translate the
      // image to that position
      double inset = bounds.getHeight();
      double inset_w = bounds.getWidth();

      watermarkStr = getWaterMark(event, WATERMARK1);
      bounds = g.getFont().getStringBounds(watermarkStr,
        g.getFontRenderContext());

      inset = bounds.getHeight();
      inset_w = bounds.getWidth();
      
      g.translate(event.getImage().getWidth() - inset_w, 
    			event.getImage().getHeight() - inset);
      //g.setColor(new Color(128, 255, 128, 56));
      g.setColor(new Color(128, 170, 200, 100));
      //g.setColor(new Color(0, 0, 255, 255));
 	  g.fill(bounds);
      g.setColor(Color.BLACK);
      g.drawString(watermarkStr, 0, 0);
      
      // call parent which will pass the video onto next tool in chain
      super.onVideoPicture(event);
    }
  }
 
  static public String getDateTime() {
      DateFormat dateFormat = new SimpleDateFormat("HH:mm:ss");
      Date date = new Date();
      return dateFormat.format(date);
  }
}

