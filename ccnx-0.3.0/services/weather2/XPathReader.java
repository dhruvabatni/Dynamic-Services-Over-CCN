import java.io.IOException;
import javax.xml.namespace.QName;
import javax.xml.parsers.*;
import javax.xml.xpath.*;
import org.w3c.dom.Document;
import org.xml.sax.SAXException;
import java.util.Iterator;
import javax.xml.namespace.NamespaceContext;

public class XPathReader {

	private String xmlFile;
	private Document xmlDocument;
	private XPath xPath;

	public XPathReader(String xmlFile) {
		this.xmlFile = xmlFile;
		initObjects();
	}

	private void initObjects(){        
		try {
			DocumentBuilderFactory xmlFact = 
				DocumentBuilderFactory.newInstance();
			xmlFact.setNamespaceAware(true);
			xmlFact.setValidating(false);
			xmlFact.setFeature("http://apache.org/xml/features/nonvalidating/load-external-dtd", false);

			xmlDocument = xmlFact.newDocumentBuilder().parse(xmlFile);            

			NamespaceContext ctx = new NamespaceContext() {
				public String getNamespaceURI(String prefix) {
					String uri;
					if (prefix.equals("yweather"))
						uri = "http://xml.weather.yahoo.com/ns/rss/1.0";
					else
						uri = null;
					return uri;
				}

				public Iterator getPrefixes(String val) {
					return null;
				}

				public String getPrefix(String uri) {
					return null;
				}
			};

			xPath =  XPathFactory.newInstance().newXPath();
			xPath.setNamespaceContext(ctx);
		} catch (IOException ex) {
			ex.printStackTrace();
		} catch (SAXException ex) {
			ex.printStackTrace();
		} catch (ParserConfigurationException ex) {
			ex.printStackTrace();
		}       
	}

	public Object read(String expression, 
			QName returnType){
		try {
			XPathExpression xPathExpression = 
				xPath.compile(expression);
			return xPathExpression.evaluate
			(xmlDocument, returnType);
		} catch (XPathExpressionException ex) {
			ex.printStackTrace();
			return null;
		}
	}

	public static void main(String[] args){
		XPathReader reader = new XPathReader("http://weather.yahooapis.com/forecastrss?w=615702");

		// To get a xml attribute.
		String expression = "rss/channel/item/yweather:condition/@text";
		System.out.println("Text = " + reader.read(expression, 
				XPathConstants.STRING) + "\n");

		expression = "rss/channel/item/yweather:condition/@temp";
		System.out.println("Temp = " + reader.read(expression, 
				XPathConstants.STRING) + "\n");

		expression = "rss/channel/item/yweather:condition/@date";
		System.out.println("Date = " + reader.read(expression, 
				XPathConstants.STRING) + "\n");

	}

}

