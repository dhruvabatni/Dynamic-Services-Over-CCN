import java.io.BufferedWriter;
import java.io.FileWriter;
import java.io.IOException;

//import javax.xml.xpath.XPathConstants;

public class news {
    public String run_news (String file) {
        smilFileNewsService sf = new smilFileNewsService();
        return (sf.getSmilFileWithOverlays(file));
    }
}

