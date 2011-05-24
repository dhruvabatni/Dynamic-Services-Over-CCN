import java.io.BufferedWriter;
import java.io.FileWriter;
import java.io.IOException;
import java.io.*;

//import javax.xml.xpath.XPathConstants;

public class weather2 {
	public String run_weather2 (String file) {
        CacheVideo c = new CacheVideo(file);
        c.processVideo();
        return null;
	}
}
