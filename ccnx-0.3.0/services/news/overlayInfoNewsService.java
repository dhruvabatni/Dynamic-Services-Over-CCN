import java.io.*;
import java.util.*;
import java.text.*;
import java.net.*;

import org.w3c.dom.*;
import java.text.*;
import javax.xml.xpath.*;

public class overlayInfoNewsService {
    public String getNewsFeed(){
        String news = "";
        for (int i=0; i<=10; i++) {
            news += "  ";
        }
        XPathReader reader = new XPathReader("http://www.bbc.co.uk/news/rss.xml");
        String expression = "//item/title/text()";

        Object result = reader.read(expression,XPathConstants.NODESET);
        NodeList nodes = (NodeList) result;
        for (int i = 0; i < nodes.getLength(); i++) {
            news += nodes.item(i).getNodeValue() + " * ";
        }
        return (news);
    }
}
