import java.io.*;
import java.util.*;
import java.text.*;
import java.net.*;

import org.w3c.dom.*;
import java.text.*;
import javax.xml.xpath.*;

public class overlayInfo {

    private static final String API_KEY = "537ccace949f76e156874d03992c33f8263c9ab9c1b08bb0c56dadb21bef5707";
    String wString1 = null, wString2 = null;
    private static String nsLocation = null;
    float latitude = (float)40.7880;
    float longitude = (float)-73.9712;

    public void setLocation () {
        nsLocation = "lat=" + latitude + "&lon=" + longitude;
    }

    /** lookup longitude and latidude depending on the ip address
     *  @param void
     *  @return array of 2 elements - longitude and latitude
     */
    private float[] lookupLatitudeLongitude() {
        String[] tmp = new String[2];
        float[] coord = new float[2];

        XPathReader reader = new XPathReader("http://api.ipinfodb.com/v2/ip_query.php?key=" + API_KEY);

        String expression = "Response/Latitude";
        tmp[0] = reader.read(expression,XPathConstants.STRING).toString();

        expression = "Response/Longitude";
        tmp[1] = reader.read(expression,XPathConstants.STRING).toString();

        coord[0] = Float.valueOf(tmp[0]).floatValue();
        coord[1] = Float.valueOf(tmp[1]).floatValue();

        return (coord);
    }

    /** Do a weather query from weather.gov
     *  @param void
     *  @return a string with latest weather information
     */
    public String lookupWeather() {
        float[] coord = lookupLatitudeLongitude();

        latitude = coord[0];
        longitude = coord[1];
        setLocation();

        XPathReader reader = new XPathReader("http://forecast.weather.gov/MapClick.php?" + nsLocation + "&FcstType=dwml");

        String wPlace = reader.read("dwml/data/location/description",
                XPathConstants.STRING).toString();
        String wText = reader.read("dwml/data/parameters/weather/weather-conditions/@weather-summary",
                XPathConstants.STRING).toString();
        String wTemp = reader.read("dwml/data/parameters/temperature/value",
                XPathConstants.STRING).toString();
        String wDate = reader.read("dwml/data/time-layout/start-valid-time",
                XPathConstants.STRING).toString();

        // Cut off everything in location after comma
        wPlace = wPlace.substring(0, wPlace.indexOf(","));

        // Get current time
        Date now = new Date();
        DateFormat dfm = new SimpleDateFormat("h:mm a");
        wString1 = wPlace + ", " + dfm.format(now);
        wString2 = wText + ", " + wTemp + " F";//" ºF";
        if ((wText.length() + wTemp.length() + wDate.length()) == 0)
            return null;
        else
            return wString1 + ", " + wString2;

    }
}
